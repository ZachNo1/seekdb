#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    const ParseNode *found = find_node(node->children_[i], type);
    if (found != nullptr) return found;
  }
  return nullptr;
}
}

TEST(FileSqlParser, file_functions_aliases_and_named_parameters)
{
  for (const char *sql : {
      "SELECT * FROM read_csv('/tmp/a.csv', header=true, sample_size=-1) AS a",
      "SELECT * FROM read_json('/tmp/a.jsonl', format='newline_delimited') j",
      "SELECT * FROM read_ndjson('/tmp/a.jsonl')",
      "SELECT * FROM read_parquet('/tmp/a.parquet') p",
      "SELECT file FROM glob('/tmp/*.csv') g"}) {
    ObArenaAllocator allocator("FileSqlParser");
    ObParser parser(allocator, DEFAULT_MYSQL_MODE);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
    const ParseNode *file = find_node(result.result_tree_, T_FILE_TABLE_EXPRESSION);
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(3, file->num_child_);
    ASSERT_NE(nullptr, file->children_[1]);
    EXPECT_GE(file->children_[1]->num_child_, 1);
    EXPECT_EQ(T_VARCHAR, file->children_[1]->children_[0]->type_);
  }
}

TEST(FileSqlParser, describe_marker_does_not_change_bind_count)
{
  for (const char *sql : {"EXPLAIN SELECT 1", "DESCRIBE SELECT 1", "DESC SELECT 1",
                          "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')"}) {
    ObArenaAllocator allocator("FileSqlParser");
    ObParser parser(allocator, DEFAULT_MYSQL_MODE);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result));
    const ParseNode *node = find_node(result.result_tree_, T_EXPLAIN);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(0, node->value_);
    EXPECT_EQ(sql[0] == 'D' ? 1U : 0U, node->reserved_);
  }
}

TEST(FileSqlParser, copy_table_and_query_options)
{
  for (const char *sql : {
      "COPY t TO '/tmp/a.csv' (FORMAT CSV, HEADER, DELIMITER ';', NULLSTR 'NULL')",
      "COPY (SELECT id FROM read_csv('/tmp/a.csv') ORDER BY id) TO '/tmp/b.csv' (HEADER false)"}) {
    ObArenaAllocator allocator("FileSqlParser");
    ObParser parser(allocator, DEFAULT_MYSQL_MODE);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
    const ParseNode *into = find_node(result.result_tree_, T_INTO_OUTFILE);
    ASSERT_NE(nullptr, into);
    EXPECT_EQ(1, into->value_);
    EXPECT_EQ(2, into->num_child_);
  }
}
