/** Copyright (c) 2026 OceanBase. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/ob_sql_context.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;

int AiColumnDml::identifier(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("`"));
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    if (value.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(value.ptr() + i, 1));
  }
  OZ(sql.append("`"));
  return ret;
}

int AiColumnDml::literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  // Hex literals are independent of NO_BACKSLASH_ESCAPES and preserve NULs.
  OZ(sql.append("X'"));
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    OZ(sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i])));
  }
  OZ(sql.append("'"));
  return ret;
}

namespace {
int ai_row_predicate(const ObTableSchema &table, const ObDMLBaseCtDef &ctdef,
                     const ExprFixedArray &row, ObEvalCtx &ctx, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_column_num(); ++i) {
    uint64_t cid = table.get_rowkey_info().get_column(i)->column_id_;
    int64_t index = -1;
    for (int64_t j = 0; j < ctdef.column_ids_.count(); ++j) {
      if (ctdef.column_ids_.at(j) == cid) { index = j; break; }
    }
    const ObColumnSchemaV2 *column = table.get_column_schema(cid);
    if (index < 0 || index >= row.count() || column == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (i > 0) { OZ(predicate.append(" AND ")); }
      OZ(AiColumnDml::identifier(predicate, column->get_column_name_str()));
      OZ(predicate.append(" = "));
      ObDatum *datum = nullptr;
      ObObj value;
      OZ(row.at(index)->eval(ctx, datum));
      OZ(datum->to_obj(value, row.at(index)->obj_meta_));
      if (OB_SUCC(ret) && value.is_string_type()) {
        OZ(AiColumnDml::literal(predicate, value.get_string()));
      } else if (OB_SUCC(ret)) {
        char buffer[512];
        int64_t pos = 0;
        OZ(value.print_sql_literal(buffer, sizeof(buffer), pos));
        OZ(predicate.append(buffer, pos));
      }
    }
  }
  if (OB_SUCC(ret) && (predicate.empty() || predicate.length() > 16384)) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}

bool ai_updated(const ObUpdCtDef &ctdef, uint64_t column_id)
{
  for (int64_t i = 0; i < ctdef.dupd_ctdef_.updated_column_ids_.count(); ++i) {
    if (ctdef.dupd_ctdef_.updated_column_ids_.at(i) == column_id) { return true; }
  }
  return false;
}

ObString ai_json_string(ObJsonObject &object, const char *key)
{
  ObJsonNode *node = object.get_value(key);
  return node != nullptr && node->json_type() == ObJsonNodeType::J_STRING
      ? static_cast<ObJsonString *>(node)->get_str() : ObString();
}
}

int AiColumnDml::check_strict_read(ObExecContext &ctx, uint64_t table_id, const UIntFixedArray &columns)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObSQLSessionInfo *session = ctx.get_my_session();
  OZ(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled));
  if (OB_FAIL(ret) || !enabled || is_inner_table(table_id)) { return ret; }
  ObSchemaGetterGuard *schema = ctx.get_sql_ctx()->schema_guard_;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  OZ(schema->get_table_schema(table_id, table));
  CK(table != nullptr);
  if (OB_FAIL(ret)) { return ret; }
  ObSEArray<uint64_t, 4> referenced;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema(columns.at(i));
    if (column != nullptr && !column->is_hidden()
        && AiColumnResolverUtils::is_ai_column(*table, column->get_column_name_str())) {
      OZ(referenced.push_back(column->get_column_id()));
    }
  }
  if (OB_FAIL(ret) || referenced.empty()) { return ret; }
  OZ(schema->get_database_schema(table->get_database_id(), database));
  CK(database != nullptr);
  sqlclient::ObISQLConnectionGuard connection_guard;
  ObIInnerSQLConnection *connection = nullptr;
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  bool nested = false;
  if (OB_SUCC(ret)) {
    if (session->get_inner_conn() != nullptr) {
      connection = as_inner_sql_connection(session->get_inner_conn());
    } else {
      OZ(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection_guard));
      connection = as_inner_sql_connection(connection_guard.get_ptr());
    }
    CK(connection != nullptr);
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(connection->begin_nested_session(saved_session, saved_connection, true))) {
    } else { nested = true; }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < referenced.count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema(referenced.at(i));
    ObSqlString query, state;
    OZ(AiColumnResolverUtils::hidden_name(column->get_column_name_str(), 's', state));
    OZ(query.append("SELECT 1 FROM "));
    OZ(identifier(query, database->get_database_name_str()));
    OZ(query.append("."));
    OZ(identifier(query, table->get_table_name_str()));
    OZ(query.append(" WHERE "));
    OZ(identifier(query, state.string()));
    OZ(query.append(" IS NULL OR "));
    OZ(identifier(query, state.string()));
    OZ(query.append("<>1 LIMIT 1"));
    ObMySQLProxy::MySQLResult result;
    OZ(connection->execute_read(query.ptr(), result));
    if (OB_SUCC(ret)) {
      int next_ret = result.get_result()->next();
      if (next_ret == OB_SUCCESS) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, column->get_column_id());
      } else if (next_ret != OB_ITER_END) { ret = next_ret; }
    }
  }
  if (nested) {
    int end_ret = connection->end_nested_session(saved_session, saved_connection);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int AiColumnDml::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  if (!ctdef.has_ai_columns_ || !ctdef.is_primary_index_) { return ret; }
  ObArenaAllocator allocator("AiColumnDml");
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSchemaGetterGuard *guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  OZ(guard->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table));
  CK(table != nullptr);
  if (OB_FAIL(ret)) { return ret; }
  OZ(guard->get_database_schema(table->get_database_id(), database));
  CK(database != nullptr);
  const bool deleting = event == ObDmlEventType::DE_DELETING;
  const bool updating = event == ObDmlEventType::DE_UPDATING;
  ObSEArray<ObJsonObject *, 4> selected;
  bool key_changed = false;
  if (updating) {
    const ObUpdCtDef &update = static_cast<const ObUpdCtDef &>(ctdef);
    for (int64_t i = 0; i < table->get_rowkey_column_num(); ++i) {
      key_changed |= ai_updated(update, table->get_rowkey_info().get_column(i)->column_id_);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && !deleting && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (!AiColumnResolverUtils::is_spec_column(*column)) { continue; }
    ObString definition = column->get_cur_default_value().get_string();
    ObJsonObject *spec = nullptr;
    OZ(ObAIFuncJsonUtils::get_json_object_form_str(allocator, definition, spec));
    bool affected = !updating || key_changed;
    if (updating && ai_updated(static_cast<const ObUpdCtDef &>(ctdef), column->get_column_id())) {
      // RETRY explicitly assigns only the selected result-spec column.
      affected = true;
    }
    if (OB_SUCC(ret) && !affected) {
      ObJsonNode *node = spec->get_value("dependencies");
      CK(node != nullptr && node->json_type() == ObJsonNodeType::J_ARRAY);
      if (OB_SUCC(ret)) {
        ObJsonArray *deps = static_cast<ObJsonArray *>(node);
        for (int64_t j = 0; OB_SUCC(ret) && !affected && j < deps->element_count(); ++j) {
          ObString name = static_cast<ObJsonString *>(deps->get_value(j))->get_str();
          const ObColumnSchemaV2 *dependency = table->get_column_schema(name);
          CK(dependency != nullptr);
          if (OB_SUCC(ret)) { affected = ai_updated(static_cast<const ObUpdCtDef &>(ctdef), dependency->get_column_id()); }
        }
      }
    }
    if (affected) { OZ(selected.push_back(spec)); }
  }
  if (OB_FAIL(ret) || (!deleting && selected.empty())) { return ret; }
  ObSqlString old_predicate, predicate, qualified;
  if (deleting || updating) { OZ(ai_row_predicate(*table, ctdef, ctdef.old_row_, op.get_eval_ctx(), old_predicate)); }
  if (!deleting) { OZ(ai_row_predicate(*table, ctdef, ctdef.new_row_, op.get_eval_ctx(), predicate)); }
  OZ(identifier(qualified, database->get_database_name_str()));
  OZ(qualified.append("."));
  OZ(identifier(qualified, table->get_table_name_str()));

  // Prepare all parent-row values before entering nested execution.
  char uuid[UuidCommon::LENGTH_UUID];
  if (!deleting) { OZ(ObExprUuid::gen_server_uuid(uuid, sizeof(uuid))); }
  ObString request_id(sizeof(uuid), uuid);
  bool nested = false;
  OZ(op.open_inner_conn());
  if (OB_SUCC(ret)) {
    if (OB_FAIL(op.begin_nested_session(true))) {
    } else { nested = true; }
  }
  if (OB_SUCC(ret) && (deleting || key_changed)) {
    ObSqlString sql;
    OZ(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=", table->get_table_id()));
    OZ(literal(sql, old_predicate.string()));
    OZ(op.execute_write(sql.ptr()));
  }
  ObSqlString invalidate;
  if (OB_SUCC(ret) && !deleting) {
    OZ(invalidate.append_fmt("UPDATE %s SET ", qualified.ptr()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < selected.count(); ++i) {
    ObJsonObject &spec = *selected.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(ai_json_string(spec, "column_name"));
    CK(column != nullptr);
    if (OB_FAIL(ret)) { break; }
    ObSqlString remove;
    OZ(remove.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND rowkey_predicate=",
                        table->get_table_id(), column->get_column_id()));
    OZ(literal(remove, predicate.string()));
    OZ(op.execute_write(remove.ptr()));
    if (i > 0) { OZ(invalidate.append(",")); }
    OZ(identifier(invalidate, column->get_column_name_str()));
    OZ(invalidate.append("=NULL,"));
    OZ(identifier(invalidate, ai_json_string(spec, "state_column")));
    OZ(invalidate.append("=0,"));
    OZ(identifier(invalidate, ai_json_string(spec, "request_column")));
    OZ(invalidate.append("="));
    OZ(literal(invalidate, request_id));
  }
  if (OB_SUCC(ret) && !deleting) {
    OZ(invalidate.append_fmt(" WHERE %s", predicate.ptr()));
    OZ(op.execute_write(invalidate.ptr()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < selected.count(); ++i) {
    ObJsonObject &spec = *selected.at(i);
    const ObColumnSchemaV2 *column = table->get_column_schema(ai_json_string(spec, "column_name"));
    const ObColumnSchemaV2 *spec_column = table->get_column_schema(ai_json_string(spec, "result_spec_column"));
    ObJsonNode *args_node = spec.get_value("arguments");
    CK(column != nullptr && spec_column != nullptr && args_node != nullptr && args_node->json_type() == ObJsonNodeType::J_ARRAY);
    if (OB_FAIL(ret)) { break; }
    ObJsonArray &args = *static_cast<ObJsonArray *>(args_node);
    ObString model = static_cast<ObJsonString *>(args.get_value(0))->get_str();
    ObString prompt = static_cast<ObJsonString *>(args.get_value(1))->get_str();
    ObString config = ObString::make_string("NULL");
    if (!column->is_collection() && args.element_count() == 3) {
      config = static_cast<ObJsonString *>(args.get_value(2))->get_str();
    }
    ObSqlString sql;
    OZ(sql.append("INSERT INTO oceanbase.__all_ai_column_task(table_id,column_id,request_id,eligible_ts,rowkey_predicate,state,retry_count,schema_version,definition,model_name,`input`,config) SELECT "));
    OZ(sql.append_fmt("%lu,%lu,", table->get_table_id(), column->get_column_id()));
    OZ(literal(sql, request_id));
    OZ(sql.append(",NOW(6),"));
    OZ(literal(sql, predicate.string()));
    OZ(sql.append_fmt(",0,0,%ld,", table->get_schema_version()));
    OZ(literal(sql, spec_column->get_cur_default_value().get_string()));
    OZ(sql.append_fmt(",(%.*s),(%.*s),(%.*s) FROM %s WHERE %s", model.length(), model.ptr(),
                     prompt.length(), prompt.ptr(), config.length(), config.ptr(), qualified.ptr(), predicate.ptr()));
    OZ(op.execute_write(sql.ptr()));
  }
  if (nested) {
    int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
}}
