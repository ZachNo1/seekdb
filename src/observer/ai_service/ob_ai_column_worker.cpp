/** Copyright (c) 2026 OceanBase. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ob_ai_column_worker.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/thread/ob_thread_name.h"
#include <charconv>
#include <cmath>

namespace oceanbase { namespace observer {
using namespace common;
using namespace share;
using namespace share::schema;
using namespace sql;
namespace {
struct AiPendingTask
{
  int64_t column_id = 0, state = 0, retry_count = 0, schema_version = 0;
  int64_t eligible = 0, input_null = 0, result_null = 0;
  ObString predicate, definition, model, input, config, result;
};

ObString ai_worker_string(ObJsonObject &object, const char *key)
{
  ObJsonNode *node = object.get_value(key);
  return node != nullptr && node->json_type() == ObJsonNodeType::J_STRING
      ? static_cast<ObJsonString *>(node)->get_str() : ObString();
}

int ai_copy_field(sqlclient::ObMySQLResult &row, const char *field, ObIAllocator &allocator, ObString &value)
{
  int ret = OB_SUCCESS;
  ObString data;
  OZ(row.get_varchar(field, data));
  if (OB_SUCC(ret) && data.length() > 4 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
  OZ(ob_write_string(allocator, data, value));
  return ret;
}

int ai_model_result(ObArenaAllocator &allocator, const ObColumnSchemaV2 &column,
                    ObJsonObject &spec, AiPendingTask &task)
{
  int ret = OB_SUCCESS;
  if (task.input_null) {
    task.result_null = 1;
    return ret;
  }
  ObAIFuncExprInfo *info = nullptr;
  ObAiModelEndpointInfo endpoint;
  OZ(ObAIFuncUtils::get_ai_func_info(allocator, task.model, info));
  OZ(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(allocator, task.model, endpoint));
  ObJsonObject *config = nullptr;
  ObJsonObject embed_config(&allocator);
  ObJsonInt dimension(0);
  if (OB_SUCC(ret) && column.is_collection()) {
    ObJsonNode *dim = spec.get_value("dimension");
    CK(dim != nullptr && dim->json_type() == ObJsonNodeType::J_INT);
    if (OB_SUCC(ret)) {
      dimension.set_value(dim->get_int());
      OZ(embed_config.add("dimension", &dimension));
      config = &embed_config;
    }
  } else if (OB_SUCC(ret) && !task.config.empty() && task.config != ObString::make_string("null")) {
    OZ(ObAIFuncJsonUtils::get_json_object_form_str(allocator, task.config, config));
  }
  if (OB_SUCC(ret)) {
    ObAIFuncModel model(allocator, *info, endpoint);
    ObString response;
    if (column.is_collection()) {
      OZ(model.call_dense_embedding(task.input, config, response));
      task.result = response;
    } else {
      OZ(model.call_completion(task.input, config, response, ai_worker_string(spec, "system_prompt")));
      // call_completion already returns the provider's decoded text.
      // Only JSON result columns parse that text as JSON below.
      if (OB_SUCC(ret)) { task.result = response; }
    }
  }
  if (OB_SUCC(ret) && task.result.length() > 4 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
  if (OB_SUCC(ret) && column.is_json()) {
    ObIJsonBase *json = nullptr;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, task.result, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
  } else if (OB_SUCC(ret) && ob_is_integer_type(column.get_data_type())) {
    const ObObjType type = column.get_data_type();
    const char *end = task.result.ptr() + task.result.length();
    if (type >= ObUTinyIntType && type <= ObUInt64Type) {
      uint64_t integer = 0;
      auto parsed = std::from_chars(task.result.ptr(), end, integer);
      if (parsed.ec != std::errc() || parsed.ptr != end || integer > UINT_MAX_VAL[type]) { ret = OB_INVALID_DATA; }
    } else {
      int64_t integer = 0;
      auto parsed = std::from_chars(task.result.ptr(), end, integer);
      if (parsed.ec != std::errc() || parsed.ptr != end
          || integer < INT_MIN_VAL[type] || integer > INT_MAX_VAL[type]) { ret = OB_INVALID_DATA; }
    }
  } else if (OB_SUCC(ret) && ob_is_numeric_type(column.get_data_type())) {
    double number = 0;
    const char *end = task.result.ptr() + task.result.length();
    auto parsed = std::from_chars(task.result.ptr(), end, number);
    if (parsed.ec != std::errc() || parsed.ptr != end || !std::isfinite(number)
        || ((column.get_data_type() == ObFloatType || column.get_data_type() == ObUFloatType)
            && !std::isfinite(static_cast<float>(number)))) { ret = OB_INVALID_DATA; }
  } else if (OB_SUCC(ret) && ob_is_string_type(column.get_data_type())) {
    const int64_t length = column.get_length_semantics() == LS_BYTE ? task.result.length()
        : ObCharset::strlen_char(column.get_collation_type(), task.result.ptr(), task.result.length());
    if (length > column.get_data_length()) { ret = OB_ERR_DATA_TOO_LONG; }
  }
  return ret;
}

int ai_task_where(ObSqlString &sql, int64_t table_id, const ObString &request, int64_t column_id = 0)
{
  int ret = OB_SUCCESS;
  OZ(sql.append_fmt(" WHERE table_id=%ld AND request_id=", table_id));
  OZ(AiColumnDml::literal(sql, request));
  if (column_id != 0) { OZ(sql.append_fmt(" AND column_id=%ld", column_id)); }
  return ret;
}
}

int AiColumnWorker::start_worker(ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  proxy_ = &proxy;
  set_run_wrapper(share::server_runtime());
  OZ(share::ObThreadPool::init());
  OZ(share::ObThreadPool::start());
  return ret;
}

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  while (!has_set_stop()) {
    int ret = process_one_group();
    if (ret != OB_SUCCESS && ret != OB_ITER_END && REACH_TIME_INTERVAL(10000000)) {
      LOG_WARN("AI column task processing failed", K(ret));
    }
    if (ret != OB_SUCCESS) { ob_usleep(100000); }
  }
}

int AiColumnWorker::process_one_group()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnWorker");
  ObString request;
  int64_t table_id = 0;
  {
    ObMySQLProxy::MySQLResult result;
    const char *query = "SELECT table_id, request_id FROM oceanbase.__all_ai_column_task "
        "GROUP BY table_id,request_id HAVING "
        "SUM(state=0 OR (state=3 AND eligible_ts<=NOW(6)))>0 OR SUM(state IN (2,4))=COUNT(*) "
        "ORDER BY MIN(eligible_ts),request_id LIMIT 1";
    OZ(proxy_->read(result, query));
    if (OB_SUCC(ret)) { ret = result.get_result()->next(); }
    OZ(result.get_result()->get_int("table_id", table_id));
    OZ(ai_copy_field(*result.get_result(), "request_id", allocator, request));
  }
  if (OB_FAIL(ret)) { return ret; }
  ObSEArray<AiPendingTask, 4> tasks;
  {
    ObSqlString sql;
    OZ(sql.append("SELECT column_id,state,retry_count,schema_version,rowkey_predicate,definition,model_name,"
                  "IFNULL(`input`,'') AS `input`,`input` IS NULL AS input_null,IFNULL(config,'') AS config,"
                  "IFNULL(result,'') AS result,result_is_null,eligible_ts<=NOW(6) AS eligible "
                  "FROM oceanbase.__all_ai_column_task"));
    OZ(ai_task_where(sql, table_id, request));
    OZ(sql.append(" ORDER BY column_id LIMIT 65"));
    ObMySQLProxy::MySQLResult result;
    OZ(proxy_->read(result, sql.ptr()));
    while (OB_SUCC(ret) && OB_SUCC(ret = result.get_result()->next())) {
      sqlclient::ObMySQLResult &row = *result.get_result();
      AiPendingTask task;
      OZ(row.get_int("column_id", task.column_id));
      OZ(row.get_int("state", task.state));
      OZ(row.get_int("retry_count", task.retry_count));
      OZ(row.get_int("schema_version", task.schema_version));
      OZ(row.get_int("eligible", task.eligible));
      OZ(row.get_int("input_null", task.input_null));
      OZ(row.get_int("result_is_null", task.result_null));
      OZ(ai_copy_field(row, "rowkey_predicate", allocator, task.predicate));
      OZ(ai_copy_field(row, "definition", allocator, task.definition));
      OZ(ai_copy_field(row, "model_name", allocator, task.model));
      OZ(ai_copy_field(row, "input", allocator, task.input));
      OZ(ai_copy_field(row, "config", allocator, task.config));
      OZ(ai_copy_field(row, "result", allocator, task.result));
      OZ(tasks.push_back(task));
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  if (OB_FAIL(ret) || tasks.empty()) { return OB_FAIL(ret) ? ret : OB_ITER_END; }
  if (tasks.count() > 64) { return OB_SIZE_OVERFLOW; }
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && table != nullptr) { OZ(guard.get_database_schema(table->get_database_id(), database)); }
  bool obsolete = table == nullptr || database == nullptr;
  bool terminal = true;
  ObSEArray<ObJsonObject *, 4> specs;
  for (int64_t i = 0; OB_SUCC(ret) && !obsolete && i < tasks.count(); ++i) {
    AiPendingTask &task = tasks.at(i);
    ObJsonObject *spec = nullptr;
    OZ(ObAIFuncJsonUtils::get_json_object_form_str(allocator, task.definition, spec));
    OZ(specs.push_back(spec));
    if (OB_FAIL(ret)) { break; }
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id);
    const ObColumnSchemaV2 *spec_column = table->get_column_schema(ai_worker_string(*spec, "result_spec_column"));
    obsolete = column == nullptr || spec_column == nullptr
        || !AiColumnResolverUtils::is_spec_column(*spec_column)
        || spec_column->get_cur_default_value().get_string() != task.definition;
    if (obsolete) { break; }
    if ((task.state == 0 || task.state == 3) && task.eligible) {
      if (has_set_stop()) { return OB_CANCELED; }
      int call_ret = ai_model_result(allocator, *column, *spec, task);
      ObSqlString update;
      if (call_ret == OB_SUCCESS) {
        task.state = 2;
        OZ(update.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=2,result_is_null=%ld,result=", task.result_null));
        OZ(AiColumnDml::literal(update, task.result));
      } else {
        ObJsonNode *max_retries = spec->get_value("max_retries");
        CK(max_retries != nullptr && max_retries->json_type() == ObJsonNodeType::J_INT);
        if (OB_FAIL(ret)) { break; }
        ++task.retry_count;
        task.state = task.retry_count <= max_retries->get_int() ? 3 : 4;
        // Persist a delayed retry so failure state is observable and survives restart.
        OZ(update.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%ld,retry_count=%ld,"
                            "eligible_ts=DATE_ADD(NOW(6),INTERVAL 2 SECOND),last_error='%d'",
                            task.state, task.retry_count, call_ret));
      }
      OZ(ai_task_where(update, table_id, request, task.column_id));
      int64_t affected = 0;
      OZ(proxy_->write(update.ptr(), affected));
      if (OB_SUCC(ret) && affected == 0) { return OB_SUCCESS; }
    }
    terminal &= task.state == 2 || task.state == 4;
  }
  if (OB_FAIL(ret)) { return ret; }
  if (!obsolete && !terminal) { return OB_ITER_END; }
  ObMySQLTransaction transaction;
  OZ(transaction.start(proxy_));
  int64_t affected = 0;
  if (OB_SUCC(ret) && !obsolete) {
    ObSqlString update;
    OZ(update.append("UPDATE "));
    OZ(AiColumnDml::identifier(update, database->get_database_name_str()));
    OZ(update.append("."));
    OZ(AiColumnDml::identifier(update, table->get_table_name_str()));
    OZ(update.append(" SET "));
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      AiPendingTask &task = tasks.at(i);
      ObJsonObject &spec = *specs.at(i);
      if (i > 0) { OZ(update.append(",")); }
      OZ(AiColumnDml::identifier(update, ai_worker_string(spec, "column_name")));
      OZ(update.append("="));
      if (task.state == 4 || task.result_null) { OZ(update.append("NULL")); }
      else {
        OZ(update.append("CONVERT("));
        OZ(AiColumnDml::literal(update, task.result));
        OZ(update.append(" USING utf8mb4)"));
      }
      OZ(update.append(","));
      OZ(AiColumnDml::identifier(update, ai_worker_string(spec, "state_column")));
      OZ(update.append_fmt("=%d", task.state == 2 ? 1 : 2));
    }
    OZ(update.append_fmt(" WHERE (%.*s)", tasks.at(0).predicate.length(), tasks.at(0).predicate.ptr()));
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      OZ(update.append(" AND "));
      OZ(AiColumnDml::identifier(update, ai_worker_string(*specs.at(i), "request_column")));
      OZ(update.append("="));
      OZ(AiColumnDml::literal(update, request));
    }
    OZ(transaction.write(update.ptr(), affected));
    if (OB_SUCC(ret) && affected == 0) {
      // Row ownership changed after loading tasks. Reload the surviving group.
      return transaction.end(false);
    }
  }
  ObSqlString cleanup;
  OZ(cleanup.append("DELETE FROM oceanbase.__all_ai_column_task"));
  OZ(ai_task_where(cleanup, table_id, request));
  OZ(transaction.write(cleanup.ptr(), affected));
  if (transaction.is_started()) {
    int end_ret = transaction.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
}}
