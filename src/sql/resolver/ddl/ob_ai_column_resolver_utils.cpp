/** Copyright (c) 2026 OceanBase. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/resolver/ob_stmt_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/dml/ob_ai_column_dml.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
const char *AI_SPEC_COMMENT = "seekdb.ai-column.v1";

ObString node_string(const ParseNode *node)
{
  return node == nullptr ? ObString() : ObString(node->str_len_, node->str_value_);
}

int ai_split_arguments(ObString expression, ObJsonArray &arguments, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  int64_t start = -1, depth = 0;
  char quote = 0;
  bool line_comment = false, block_comment = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < expression.length(); ++i) {
    char c = expression.ptr()[i];
    char next = i + 1 < expression.length() ? expression.ptr()[i + 1] : 0;
    if (line_comment) { if (c == '\n') { line_comment = false; } }
    else if (block_comment) { if (c == '*' && next == '/') { block_comment = false; ++i; } }
    else if (quote != 0) {
      if (c == '\\') { ++i; }
      else if (c == quote && next == quote) { ++i; }
      else if (c == quote) { quote = 0; }
    } else if (c == '\'' || c == '"' || c == '`') { quote = c; }
    else if (c == '#') { line_comment = true; }
    else if (c == '-' && next == '-' && i + 2 < expression.length()
             && static_cast<unsigned char>(expression.ptr()[i + 2]) <= ' ') { line_comment = true; ++i; }
    else if (c == '/' && next == '*') { block_comment = true; ++i; }
    else if (c == '(') { if (depth++ == 0) { start = i + 1; } }
    else if ((c == ',' && depth == 1) || (c == ')' && depth == 1)) {
      ObString part(i - start, expression.ptr() + start);
      ObJsonString *value = nullptr;
      OZ(ObAIFuncJsonUtils::get_json_string(allocator, part, value));
      OZ(arguments.array_append(value));
      start = i + 1;
      if (c == ')') { --depth; }
    } else if (c == ')') { --depth; }
  }
  if (OB_SUCC(ret) && (depth != 0 || quote != 0 || block_comment
                      || arguments.element_count() < 2 || arguments.element_count() > 3)) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int validate_dependencies(const ParseNode *node, const ObTableSchema &table,
                         const ObIArray<ObString> &ai_columns,
                         ObJsonArray &dependencies, ObIAllocator &allocator,
                         int depth = 0)
{
  int ret = OB_SUCCESS;
  if (node == nullptr) {
  } else if (depth > 128) {
    ret = OB_SIZE_OVERFLOW;
  } else if (node->type_ == T_COLUMN_REF) {
    if (node->num_child_ != 3 || node->children_[0] != nullptr || node->children_[1] != nullptr) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "qualified AI column dependencies");
    } else {
      ObString name = node_string(node->children_[2]);
      for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
        if (name.case_compare(ai_columns.at(i)) == 0) {
          ret = OB_NOT_SUPPORTED;
          FORWARD_USER_ERROR(OB_NOT_SUPPORTED, ob_strerror(OB_NOT_SUPPORTED));
        }
      }
      if (OB_SUCC(ret) && AiColumnResolverUtils::is_ai_column(table, name)) {
        ret = OB_NOT_SUPPORTED;
        FORWARD_USER_ERROR(OB_NOT_SUPPORTED, ob_strerror(OB_NOT_SUPPORTED));
      }
      const ObColumnSchemaV2 *dependency = table.get_column_schema(name);
      if (OB_SUCC(ret) && (dependency == nullptr || dependency->is_hidden())) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.length(), name.ptr(), "AI COLUMN");
      }
      if (OB_SUCC(ret)) {
        ObJsonString *json_name = nullptr;
        OZ(ObAIFuncJsonUtils::get_json_string(allocator, name, json_name));
        OZ(dependencies.array_append(json_name));
      }
    }
  } else if (node->type_ == T_SELECT || node->type_ == T_QUESTIONMARK
             || node->type_ == T_OP_ASSIGN || node->type_ == T_SYSTEM_VARIABLE
             || node->type_ == T_USER_VARIABLE_IDENTIFIER) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "subqueries, parameters or variables in AI columns");
  } else {
    if (node->type_ == T_FUN_SYS && node->num_child_ > 0) {
      ObString name = node_string(node->children_[0]);
      if (name.case_compare("AI_COMPLETE") == 0 || name.case_compare("AI_EMBED") == 0) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested AI calls in AI columns");
      }
    }
    for (int i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      OZ(validate_dependencies(node->children_[i], table, ai_columns, dependencies, allocator, depth + 1));
    }
  }
  return ret;
}
}

bool AiColumnResolverUtils::internal_session(ObSQLSessionInfo &session)
{
  // Ordinary client SQL (including PL SQL) cannot opt into internal AI writes.
  ObExecContext *ctx = session.get_cur_exec_ctx();
  return !session.is_user_session()
      || (session.is_inner() && ctx != nullptr && ctx->get_parent_ctx() != nullptr
          && ctx->get_parent_ctx()->get_pl_stack_ctx() == nullptr);
}

int AiColumnResolverUtils::rewrite_show(ObResolverParams &params, const ParseNode &node, const ParseNode *&rewritten)
{
  int ret = OB_SUCCESS;
  const ParseNode *relation = node.children_[1];
  ObString database = relation->children_[0] == nullptr ? params.session_info_->get_database_name()
                                                        : node_string(relation->children_[0]);
  if (node.children_[2] != nullptr) { database = node_string(node.children_[2]); }
  const ObTableSchema *table = nullptr;
  OZ(params.schema_checker_->get_table_schema(database, node_string(relation->children_[1]), false, table));
  CK(table != nullptr);
  ObSqlString query;
  if (OB_SUCC(ret)) {
    OZ(query.append_fmt("SELECT column_name,normalized_expr,'async' AS sync_mode,max_retries,"
        "state_column_id,request_column_id,result_spec_column_id FROM oceanbase.__all_ai_column "
        "WHERE table_id=%lu ORDER BY column_id", table->get_table_id()));
  }
  ObString text;
  OZ(ob_write_string(*params.allocator_, query.string(), text));
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  OZ(parser.parse(text, parsed));
  CK(parsed.result_tree_ != nullptr && parsed.result_tree_->num_child_ == 1);
  if (OB_SUCC(ret)) { rewritten = parsed.result_tree_->children_[0]; }
  return ret;
}

int AiColumnResolverUtils::rewrite_retry(ObResolverParams &params, const ParseNode &node, const ParseNode *&rewritten)
{
  int ret = OB_SUCCESS;
  if (node.num_child_ != 4 || node.children_[1] == nullptr
      || node.children_[1]->num_child_ != 1) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
    return ret;
  }
  const ParseNode *relation = node.children_[0];
  const ParseNode *retry = node.children_[1]->children_[0];
  ObString database = relation->children_[0] == nullptr ? params.session_info_->get_database_name()
                                                        : node_string(relation->children_[0]);
  ObString name = node_string(relation->children_[1]);
  const ObTableSchema *table = nullptr;
  OZ(params.schema_checker_->get_table_schema(database, name, false, table));
  CK(table != nullptr);
  const ObColumnSchemaV2 *column = nullptr;
  if (OB_SUCC(ret)) { column = table->get_column_schema(node_string(retry->children_[0])); }
  if (OB_SUCC(ret) && (column == nullptr || !is_ai_column(*table, column->get_column_name_str()))) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  }
  ObSqlString query, spec_name, state_name, request_name;
  if (OB_FAIL(ret)) { return ret; }
  OZ(hidden_name(column->get_column_name_str(), 'r', spec_name));
  OZ(hidden_name(column->get_column_name_str(), 's', state_name));
  OZ(hidden_name(column->get_column_name_str(), 'q', request_name));
  OZ(query.append("UPDATE "));
  OZ(AiColumnDml::identifier(query, database));
  OZ(query.append("."));
  OZ(AiColumnDml::identifier(query, table->get_table_name_str()));
  OZ(query.append(" SET "));
  OZ(AiColumnDml::identifier(query, column->get_column_name_str()));
  OZ(query.append("=NULL,"));
  OZ(AiColumnDml::identifier(query, spec_name.string()));
  OZ(query.append("="));
  OZ(AiColumnDml::identifier(query, spec_name.string()));
  OZ(query.append(","));
  OZ(AiColumnDml::identifier(query, request_name.string()));
  OZ(query.append("=UUID() "));
  if (retry->children_[1] != nullptr) {
    ObString where = node_string(retry->children_[1]);
    char quote = 0;
    bool line_comment = false, block_comment = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < where.length();) {
      const char c = where.ptr()[i];
      const char next = i + 1 < where.length() ? where.ptr()[i + 1] : 0;
      if (quote != 0) {
        OZ(query.append(where.ptr() + i, 1)); ++i;
        if (c == '\\' && i < where.length()) { OZ(query.append(where.ptr() + i++, 1)); }
        else if (c == quote && next == quote) { OZ(query.append(where.ptr() + i++, 1)); }
        else if (c == quote) { quote = 0; }
      } else if (line_comment) {
        OZ(query.append(where.ptr() + i++, 1));
        if (c == '\n') { line_comment = false; }
      } else if (block_comment) {
        OZ(query.append(where.ptr() + i++, 1));
        if (c == '*' && next == '/') { OZ(query.append(where.ptr() + i++, 1)); block_comment = false; }
      } else if (c == '\'' || c == '"' || c == '`') {
        quote = c; OZ(query.append(where.ptr() + i++, 1));
      } else if (c == '#' || (c == '-' && next == '-' && i + 2 < where.length()
                 && static_cast<unsigned char>(where.ptr()[i + 2]) <= ' ')) {
        line_comment = true; OZ(query.append(where.ptr() + i++, 1));
      } else if (c == '/' && next == '*') {
        block_comment = true; OZ(query.append(where.ptr() + i, 2)); i += 2;
      } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
        int64_t start = i++;
        while (i < where.length()) {
          char ch = where.ptr()[i];
          if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9') || ch == '_' || ch == '$')) { break; }
          ++i;
        }
        ObString word(i - start, where.ptr() + start);
        if (word.case_compare("status") == 0) {
          OZ(query.append("(CASE "));
          OZ(AiColumnDml::identifier(query, state_name.string()));
          OZ(query.append(" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)"));
        } else { OZ(query.append(word)); }
      } else { OZ(query.append(where.ptr() + i++, 1)); }
    }
  }
  ObString text;
  OZ(ob_write_string(*params.allocator_, query.string(), text));
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  OZ(parser.parse(text, parsed));
  CK(parsed.result_tree_ != nullptr && parsed.result_tree_->num_child_ == 1);
  if (OB_SUCC(ret)) { rewritten = parsed.result_tree_->children_[0]; }
  return ret;
}

const ParseNode *AiColumnResolverUtils::definition(const ParseNode *column)
{
  const ParseNode *result = nullptr;
  if (column != nullptr && column->type_ == T_COLUMN_DEFINITION && column->num_child_ >= 3) {
    const ParseNode *attrs = column->children_[2];
    for (int i = 0; attrs != nullptr && i < attrs->num_child_; ++i) {
      if (attrs->children_[i] != nullptr && attrs->children_[i]->type_ == T_AI_COLUMN_DEFINITION) {
        result = attrs->children_[i];
        break;
      }
    }
  }
  return result;
}

bool AiColumnResolverUtils::is_spec_column(const ObColumnSchemaV2 &column)
{
  return column.is_hidden() && ObString::make_string(column.get_comment()) == ObString::make_string(AI_SPEC_COMMENT);
}

int AiColumnResolverUtils::hidden_name(const ObString &name, char kind, ObSqlString &out)
{
  // Fixed width names also support maximum-length user column identifiers.
  return out.assign_fmt("__ai_col_%016lx_%c", name.hash(), kind);
}

bool AiColumnResolverUtils::is_ai_column(const ObTableSchema &table, const ObString &name)
{
  ObSqlString spec_name;
  const ObColumnSchemaV2 *column = nullptr;
  const ObColumnSchemaV2 *result = table.get_column_schema(name);
  return result != nullptr && OB_SUCCESS == hidden_name(result->get_column_name_str(), 'r', spec_name)
      && (column = table.get_column_schema(spec_name.string())) != nullptr
      && is_spec_column(*column);
}

int AiColumnResolverUtils::build_hidden_columns(ObIAllocator &allocator,
    const ObTableSchema &table, const ObColumnSchemaV2 &column,
    const ParseNode &definition, const ObIArray<ObString> &declared_ai_columns,
    ObIArray<ObColumnSchemaV2> &columns)
{
  int ret = OB_SUCCESS;
  const ParseNode *expr = definition.children_[0];
  const ParseNode *options = definition.children_[1];
  const ParseNode *args = nullptr;
  int64_t max_retries = 3;
  bool seen_mode = false, seen_retries = false;
  bool embed = false;
  if (expr == nullptr || expr->type_ != T_FUN_SYS || expr->num_child_ != 2) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI columns without an AI_COMPLETE or AI_EMBED call");
  } else {
    ObString function_name = node_string(expr->children_[0]);
    embed = function_name.case_compare("AI_EMBED") == 0;
    args = expr->children_[1];
    if ((!embed && function_name.case_compare("AI_COMPLETE") != 0)
        || args == nullptr || args->num_child_ < 2 || args->num_child_ > 3) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  for (int i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    ObString name = node_string(option->children_[0]);
    const ParseNode *value = option->children_[1];
    if (name.case_compare("sync_mode") == 0) {
      if (seen_mode || value->type_ != T_VARCHAR || node_string(value).case_compare("async") != 0) {
        ret = OB_INVALID_ARGUMENT;
      }
      seen_mode = true;
    } else if (name.case_compare("max_retries") == 0) {
      if (seen_retries || value->type_ != T_INT || value->value_ < 0 || value->value_ > 100) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        max_retries = value->value_;
      }
      seen_retries = true;
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  if (OB_SUCC(ret) && (!column.is_nullable() || column.is_rowkey_column()
                      || column.is_autoincrement() || !column.get_cur_default_value().is_null())) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column constraints or explicit defaults");
  }
  int64_t dimension = 0;
  if (OB_SUCC(ret) && embed) {
    if (!column.is_collection()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED on a non-vector AI column");
    } else {
      OZ(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension));
      if (OB_SUCC(ret) && args->num_child_ == 3) {
        const ParseNode *dim = args->children_[2];
        if (dim->type_ != T_INT || dim->value_ != dimension) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(dimension),
                         static_cast<unsigned>(dim->value_));
        }
      }
    }
  }
  if (OB_SUCC(ret) && declared_ai_columns.count() > 64) { ret = OB_SIZE_OVERFLOW; }
  ObJsonObject spec(&allocator);
  ObJsonArray dependencies(&allocator);
  if (OB_SUCC(ret)) {
    // Model, prompt and optional config are evaluated by the native SQL engine.
    for (int i = 0; OB_SUCC(ret) && i < args->num_child_; ++i) {
      OZ(validate_dependencies(args->children_[i], table, declared_ai_columns, dependencies, allocator));
    }
  }
  ObString system_prompt;
  if (column.is_json()) {
    system_prompt = ObString::make_string("Return exactly one valid JSON value");
  } else if (ob_is_integer_type(column.get_data_type())) {
    system_prompt = ObString::make_string("Return exactly one base-10 integer");
  } else if (ob_is_numeric_type(column.get_data_type())) {
    system_prompt = ObString::make_string("Return exactly one finite number");
  }
  ObSqlString normalized;
  ObString expression = node_string(expr);
  OZ(normalized.append(expression));
  if (OB_SUCC(ret) && !system_prompt.empty()) {
    OZ(normalized.append_fmt(" /* %.*s */", system_prompt.length(), system_prompt.ptr()));
  }
  ObSqlString names[3];
  const char kinds[] = {'s', 'q', 'r'};
  for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
    OZ(hidden_name(column.get_column_name_str(), kinds[i], names[i]));
    if (OB_SUCC(ret) && table.get_column_schema(names[i].string()) != nullptr) {
      ret = OB_ERR_COLUMN_DUPLICATE;
      LOG_USER_ERROR(OB_ERR_COLUMN_DUPLICATE, names[i].length(), names[i].ptr());
    }
  }
  const char *keys[] = {"column_name", "expression", "normalized_expr", "system_prompt",
                       "state_column", "request_column", "result_spec_column"};
  ObString values[] = {column.get_column_name_str(), expression, normalized.string(), system_prompt,
                      names[0].string(), names[1].string(), names[2].string()};
  for (int i = 0; OB_SUCC(ret) && i < 7; ++i) {
    ObJsonString *value = nullptr;
    OZ(ObAIFuncJsonUtils::get_json_string(allocator, values[i], value));
    OZ(spec.add(ObString::make_string(keys[i]), value));
  }
  ObJsonInt retries(max_retries), version(1), dim(dimension);
  OZ(spec.add("max_retries", &retries));
  OZ(spec.add("version", &version));
  OZ(spec.add("dimension", &dim));
  OZ(spec.add("dependencies", &dependencies));
  ObJsonArray arguments(&allocator);
  OZ(ai_split_arguments(expression, arguments, allocator));
  OZ(spec.add("arguments", &arguments));
  ObString serialized;
  OZ(ObAIFuncJsonUtils::print_json_to_str(allocator, &spec, serialized));
  if (OB_SUCC(ret) && serialized.length() > OB_MAX_DEFAULT_VALUE_LENGTH) {
    ret = OB_SIZE_OVERFLOW;
  }
  for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
    ObColumnSchemaV2 hidden;
    hidden.set_table_id(table.get_table_id());
    hidden.set_column_id(table.get_max_used_column_id() + i + 1);
    hidden.set_is_hidden(true);
    hidden.set_nullable(true);
    hidden.set_data_type(i == 0 ? ObIntType : ObVarcharType);
    hidden.set_data_length(i == 0 ? 8 : (i == 1 ? 64 : serialized.length()));
    hidden.set_collation_type(i == 0 ? CS_TYPE_BINARY : CS_TYPE_UTF8MB4_BIN);
    OZ(hidden.set_column_name(names[i].string()));
    ObObj default_value;
    if (i == 0) {
      default_value.set_int(0); // PENDING
    } else if (i == 2) {
      default_value.set_varchar(serialized);
      default_value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      default_value.set_collation_level(CS_LEVEL_COERCIBLE);
      OZ(hidden.set_comment(ObString::make_string(AI_SPEC_COMMENT)));
    }
    OZ(hidden.set_cur_default_value(default_value, false));
    OZ(hidden.set_orig_default_value(default_value));
    OZ(columns.push_back(hidden));
  }
  return ret;
}
}} // namespace oceanbase::sql
