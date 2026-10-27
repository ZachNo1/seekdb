/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace filesql {

enum class Status { OK, END, INVALID, DENIED, MISSING, CHANGED, LIMIT, UNSUPPORTED };
enum class Format : uint8_t { CSV, JSONL, PARQUET, GLOB };
enum class Type : uint8_t { NULL_VALUE, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };

struct Error {
  Status status = Status::OK;
  std::string message;
  Status fail(Status code, const std::string &text) { status = code; message = text; return code; }
};
struct Options {
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
};
struct Column {
  std::string name;
  Type type = Type::NULL_VALUE;
  bool nullable = false;
  uint32_t max_length = 0;
  uint8_t fractional_scale = 0;
};
struct Fingerprint {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime_sec = 0, mtime_ns = 0, ctime_sec = 0, ctime_ns = 0;
  bool operator==(const Fingerprint &other) const;
};
struct Definition {
  Format format = Format::CSV;
  Options options;
  std::string path;
  std::string secure_root;
  Fingerprint fingerprint;
  std::vector<Column> columns;
  uint64_t rows = 0;
};
struct Cell {
  std::string text;
  bool is_null = true;
  Type source_type = Type::NULL_VALUE;
};

constexpr uint64_t MAX_FILE_SIZE = uint64_t{1} << 34;
constexpr size_t MAX_RECORD_SIZE = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 4096;
constexpr size_t MAX_GLOB_FILES = 10000;

const char *type_name(Type type);
bool valid_utf8(const std::string &text);
Type infer_type(const std::string &text);
Type widen_type(Type left, Type right);
bool parse_integer(const std::string &text, int64_t &value);
bool parse_boolean(const std::string &text, bool &value);
bool parse_double(const std::string &text, double &value);
bool parse_temporal(const std::string &text, bool datetime, int64_t &value);

// The returned descriptor is caller-owned. Components are opened beneath the
// canonical secure root without following symlinks after canonicalization.
Status open_checked(const std::string &path, const std::string &secure_root,
                    int &fd, std::string &canonical, Fingerprint &fingerprint, Error &error);
Status infer_schema(Definition &definition, Error &error);
Status discover_files(const std::string &pattern, const std::string &secure_root,
                      std::vector<std::string> &paths, Error &error);
Status encode_definition(const Definition &definition, std::string &encoded, Error &error);
Status decode_definition(const std::string &encoded, Definition &definition, Error &error);

class FileReader {
public:
  FileReader();
  ~FileReader();
  FileReader(const FileReader &) = delete;
  FileReader &operator=(const FileReader &) = delete;
  Status open(const Definition &definition, const std::vector<size_t> &projection, Error &error);
  Status next(std::vector<Cell> &row, Error &error);
  Status next_batch(size_t capacity, std::vector<std::vector<Cell>> &rows, Error &error);
  Status rescan(Error &error);
  Status check_unchanged(Error &error) const;
  void close();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class CsvWriter {
public:
  CsvWriter();
  ~CsvWriter();
  CsvWriter(const CsvWriter &) = delete;
  CsvWriter &operator=(const CsvWriter &) = delete;
  Status open(const Definition &definition, Error &error);
  Status append(const std::vector<Cell> &row, Error &error);
  Status finish(Error &error);
  void close();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}}} // namespace oceanbase::sql::filesql
