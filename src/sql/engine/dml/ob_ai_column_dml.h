/** Copyright (c) 2026 OceanBase. */
#pragma once
#include "sql/engine/dml/ob_dml_ctx_define.h"

namespace oceanbase { namespace sql {
class ObTableModifyOp;
class AiColumnDml
{
public:
  static int check_strict_read(ObExecContext &ctx, uint64_t table_id, const UIntFixedArray &columns);
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event);
  static int identifier(common::ObSqlString &sql, const common::ObString &value);
  static int literal(common::ObSqlString &sql, const common::ObString &value);
};
}}
