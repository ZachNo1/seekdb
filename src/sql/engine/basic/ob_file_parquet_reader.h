/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase { namespace sql { namespace filesql {
Status infer_parquet_schema(int fd, Definition &definition, Error &error);
class ParquetReader {
public:
  ParquetReader();
  ~ParquetReader();
  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;
  Status open(int fd, const Definition &definition, const std::vector<size_t> &projection, Error &error);
  Status next(std::vector<Cell> &row, Error &error);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}}}
