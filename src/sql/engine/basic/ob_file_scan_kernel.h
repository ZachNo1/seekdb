/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/ob_file_scan_utils.h"
namespace oceanbase {
namespace common { class ObIAllocator; }
namespace sql {
class ObSQLSessionInfo;
namespace filesql {
inline constexpr char COPY_PREFIX[] = "seekdb-file-copy-v1:";
int check_enabled(ObSQLSessionInfo &session);
int runtime_secure_root(common::ObIAllocator &allocator, std::string &root);
int sql_error(Status status, const Error &error);
}
}}
