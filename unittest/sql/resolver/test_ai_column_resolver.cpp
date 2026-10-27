#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;

namespace {
const ParseNode *resolver_ai_definition(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN_DEFINITION) { return node; }
  for (int i = 0; i < node->num_child_; ++i) {
    const ParseNode *found = resolver_ai_definition(node->children_[i]);
    if (found != nullptr) { return found; }
  }
  return nullptr;
}

void make_ai_test_table(ObTableSchema &table, ObColumnSchemaV2 &result)
{
  table.set_table_id(500001);
  table.set_table_type(USER_TABLE);
  table.set_max_used_column_id(17);
  ASSERT_EQ(OB_SUCCESS, table.set_table_name("ai_test"));
  ObColumnSchemaV2 body;
  body.set_table_id(table.get_table_id());
  body.set_column_id(16);
  body.set_data_type(ObVarcharType);
  body.set_data_length(100);
  body.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  body.set_nullable(true);
  ASSERT_EQ(OB_SUCCESS, body.set_column_name("body"));
  ASSERT_EQ(OB_SUCCESS, table.add_column(body));
  result.set_table_id(table.get_table_id());
  result.set_column_id(17);
  result.set_data_type(ObJsonType);
  result.set_nullable(true);
  ASSERT_EQ(OB_SUCCESS, result.set_column_name("tags"));
  ASSERT_EQ(OB_SUCCESS, table.add_column(result));
}
}

TEST(AiColumnResolver, persists_hidden_spec_and_exact_argument_text)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult parsed;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(
    "CREATE TABLE t(tags JSON AI COLUMN(AI_COMPLETE('m', CONCAT('x,y', body))) "
    "WITH(sync_mode='async',max_retries=2))"), parsed));
  const ParseNode *definition = resolver_ai_definition(parsed.result_tree_);
  ASSERT_NE(nullptr, definition);
  ObTableSchema table;
  ObColumnSchemaV2 result;
  make_ai_test_table(table, result);
  ObSEArray<ObString, 4> ai_columns;
  ASSERT_EQ(OB_SUCCESS, ai_columns.push_back(ObString::make_string("tags")));
  ObSEArray<ObColumnSchemaV2, 3> hidden;
  ASSERT_EQ(OB_SUCCESS, AiColumnResolverUtils::build_hidden_columns(
    allocator, table, result, *definition, ai_columns, hidden));
  ASSERT_EQ(3, hidden.count());
  for (int i = 0; i < 3; ++i) { EXPECT_TRUE(hidden.at(i).is_hidden()); }
  EXPECT_TRUE(AiColumnResolverUtils::is_spec_column(hidden.at(2)));
  ObColumnSchemaV2 copy;
  ASSERT_EQ(OB_SUCCESS, copy.assign(hidden.at(2)));
  EXPECT_EQ(copy.get_cur_default_value().get_string(), hidden.at(2).get_cur_default_value().get_string());
  ObString serialized = copy.get_cur_default_value().get_string();
  ObJsonObject *spec = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, serialized, spec));
  EXPECT_EQ(2, spec->get_value("max_retries")->get_int());
  auto *args = static_cast<ObJsonArray *>(spec->get_value("arguments"));
  ASSERT_EQ(2, args->element_count());
  EXPECT_EQ(ObString::make_string(" CONCAT('x,y', body)"),
            static_cast<ObJsonString *>(args->get_value(1))->get_str());
  EXPECT_EQ(ObString::make_string("Return exactly one valid JSON value"),
            static_cast<ObJsonString *>(spec->get_value("system_prompt"))->get_str());
}

TEST(AiColumnResolver, rejects_ai_dependencies_and_unknown_options)
{
  for (const char *sql : {
      "CREATE TABLE t(tags JSON AI COLUMN(AI_COMPLETE('m', tags)))",
      "CREATE TABLE t(tags JSON AI COLUMN(AI_COMPLETE('m', body)) WITH(unknown_option=2))"}) {
    ObArenaAllocator allocator("AiColumnTest");
    ObParser parser(allocator, DEFAULT_MYSQL_MODE);
    ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), parsed));
    ObTableSchema table;
    ObColumnSchemaV2 result;
    make_ai_test_table(table, result);
    ObSEArray<ObString, 4> names;
    ASSERT_EQ(OB_SUCCESS, names.push_back(ObString::make_string("tags")));
    ObSEArray<ObColumnSchemaV2, 3> hidden;
    EXPECT_NE(OB_SUCCESS, AiColumnResolverUtils::build_hidden_columns(
      allocator, table, result, *resolver_ai_definition(parsed.result_tree_), names, hidden));
    EXPECT_EQ(0, hidden.count());
  }
}
