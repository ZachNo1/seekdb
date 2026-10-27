#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_stmt_resolver.h"
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
const ParseNode *find_ai_node(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN_DEFINITION) { return node; }
  for (int i = 0; i < node->num_child_; ++i) {
    const ParseNode *found = find_ai_node(node->children_[i]);
    if (found != nullptr) { return found; }
  }
  return nullptr;
}
}

TEST(AiColumnParser, create_preserves_function_arguments)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(
    "CREATE TABLE t(id BIGINT PRIMARY KEY, body VARCHAR(100), tags JSON AI COLUMN "
    "(AI_COMPLETE('model', CONCAT('a,b', body))) WITH(sync_mode='async',max_retries=2))"), result));
  const ParseNode *ai = find_ai_node(result.result_tree_);
  ASSERT_NE(nullptr, ai);
  ASSERT_EQ(2, ai->num_child_);
  ASSERT_EQ(T_FUN_SYS, ai->children_[0]->type_);
  ASSERT_EQ(2, ai->children_[0]->children_[1]->num_child_);
  ASSERT_EQ(2, ai->children_[1]->num_child_);
}

TEST(AiColumnParser, alter_add_and_default_options)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(
    "ALTER TABLE t ADD COLUMN v VECTOR(4) AI COLUMN (AI_EMBED('model',body,4))"), result));
  const ParseNode *ai = find_ai_node(result.result_tree_);
  ASSERT_NE(nullptr, ai);
  ASSERT_EQ(nullptr, ai->children_[1]);
  ASSERT_EQ(3, ai->children_[0]->children_[1]->num_child_);
}

TEST(AiColumnParser, retry_preserves_predicate_and_marks_mixed_actions)
{
  for (const char *sql : {
      "ALTER TABLE t RETRY AI COLUMN tags WHERE status='FAILED' AND `status`='active'",
      "ALTER TABLE t RETRY AI COLUMN tags, ADD COLUMN x INT"}) {
    ObArenaAllocator allocator("AiColumnTest");
    ObParser parser(allocator, DEFAULT_MYSQL_MODE);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result));
    ASSERT_NE(nullptr, result.result_tree_);
    const ParseNode *root = result.result_tree_->children_[0];
    ASSERT_EQ(T_AI_COLUMN_RETRY, root->type_);
    const bool mixed = strstr(sql, "ADD COLUMN") != nullptr;
    EXPECT_EQ(mixed ? 2 : 1, root->children_[1]->num_child_);
    if (mixed) {
      ObResolverParams params;
      const ParseNode *rewritten = nullptr;
      EXPECT_EQ(OB_NOT_SUPPORTED, AiColumnResolverUtils::rewrite_retry(params, *root, rewritten));
      EXPECT_EQ(nullptr, rewritten);
    }
  }
}

TEST(AiColumnParser, rejects_unknown_retry_keyword)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult result;
  EXPECT_NE(OB_SUCCESS, parser.parse(ObString::make_string(
    "ALTER TABLE t REPEAT AI COLUMN tags"), result));
}


TEST(AiColumnParser, show_ai_columns_uses_show_column_shape)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult parsed;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string("SHOW AI COLUMNS FROM test.docs"), parsed));
  const ParseNode *root = parsed.result_tree_->children_[0];
  ASSERT_EQ(T_SHOW_COLUMNS, root->type_);
  ASSERT_EQ(4, root->num_child_);
  EXPECT_EQ(T_AI_COLUMN_DEFINITION, root->children_[0]->type_);
}
