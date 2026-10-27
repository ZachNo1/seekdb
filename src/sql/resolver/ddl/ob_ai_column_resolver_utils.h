/** Copyright (c) 2026 OceanBase. */
#pragma once

#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase { namespace sql {
struct ObResolverParams;
class ObSQLSessionInfo;
// The definition is owned by the schema of the hidden result-spec column.
// This gives CREATE/ALTER/DROP and schema history one atomic source of truth.
class AiColumnResolverUtils
{
public:
  static bool internal_session(ObSQLSessionInfo &session);
  static int rewrite_show(ObResolverParams &params, const ParseNode &node, const ParseNode *&rewritten);
  static int rewrite_retry(ObResolverParams &params, const ParseNode &node, const ParseNode *&rewritten);
  static const ParseNode *definition(const ParseNode *column);
  static bool is_spec_column(const share::schema::ObColumnSchemaV2 &column);
  static bool is_ai_column(const share::schema::ObTableSchema &table,
                           const common::ObString &name);
  static int hidden_name(const common::ObString &name, char kind,
                         common::ObSqlString &out);
  static int build_hidden_columns(common::ObIAllocator &allocator,
      const share::schema::ObTableSchema &table,
      const share::schema::ObColumnSchemaV2 &column,
      const ParseNode &definition,
      const common::ObIArray<common::ObString> &declared_ai_columns,
      common::ObIArray<share::schema::ObColumnSchemaV2> &columns);
};
}} // namespace oceanbase::sql
