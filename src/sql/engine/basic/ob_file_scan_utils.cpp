/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "ob_file_scan_utils.h"
#include "ob_file_parquet_reader.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase { namespace sql { namespace filesql {
namespace {
Fingerprint stamp(const struct stat &s)
{
  return {static_cast<uint64_t>(s.st_dev), static_cast<uint64_t>(s.st_ino), static_cast<uint64_t>(s.st_size),
          s.st_mtim.tv_sec, s.st_mtim.tv_nsec, s.st_ctim.tv_sec, s.st_ctim.tv_nsec};
}
std::string ascii_lower(std::string text)
{
  for (char &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return text;
}
std::string trimmed(const std::string &text)
{
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
bool under_root(const std::string &path, const std::string &root)
{
  return root == "/" || path == root || (path.size() > root.size() &&
      path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
int open_components(const std::string &absolute, int final_flags)
{
  int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  size_t start = 1;
  while (parent >= 0 && start < absolute.size()) {
    size_t end = absolute.find('/', start);
    bool last = end == std::string::npos;
    std::string part = absolute.substr(start, last ? absolute.size() - start : end - start);
    if (part.empty() || part == "." || part == "..") { ::close(parent); errno = EACCES; return -1; }
    int child = ::openat(parent, part.c_str(), O_CLOEXEC | O_NOFOLLOW |
        (last ? final_flags : O_RDONLY | O_DIRECTORY));
    int saved = errno;
    ::close(parent);
    parent = child;
    errno = saved;
    if (last) break;
    start = end + 1;
  }
  return parent;
}
Status canonical_root(const std::string &input, std::string &root, Error &error)
{
  if (input.empty() || ascii_lower(input) == "null" || input.find('\0') != std::string::npos)
    return error.fail(Status::DENIED, "file_sql: secure_file_priv disables file access");
  char *resolved = ::realpath(input.c_str(), nullptr);
  if (!resolved) return error.fail(Status::DENIED, "file_sql: invalid secure_file_priv directory");
  root.assign(resolved); ::free(resolved);
  struct stat info;
  if (::stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
    return error.fail(Status::DENIED, "file_sql: secure_file_priv must name a directory");
  return Status::OK;
}
bool numeric_syntax(const std::string &s, bool &integer)
{
  integer = true;
  size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
  if (i < s.size() && s[i] == '.') {
    integer = false; ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits; }
  }
  if (digits == 0) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    integer = false; ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t begin = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == begin) return false;
  }
  return i == s.size();
}
int64_t civil_days(int year, unsigned month, unsigned day)
{
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = year - era * 400;
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
class ByteReader {
public:
  explicit ByteReader(int fd) : fd_(fd), data_(new std::array<char, 65536>) {}
  void reset() { position_ = 0; begin_ = end_ = 0; failed_ = false; }
  int get() {
    int c = peek();
    if (c >= 0) { ++begin_; ++position_; }
    return c;
  }
  int peek() {
    if (begin_ == end_) {
      ssize_t size;
      do { size = ::pread(fd_, data_->data(), data_->size(), position_); } while (size < 0 && errno == EINTR);
      if (size < 0) failed_ = true;
      if (size <= 0) return -1;
      begin_ = 0; end_ = static_cast<size_t>(size);
    }
    return static_cast<unsigned char>((*data_)[begin_]);
  }
  bool failed() const { return failed_; }
  void skip_bom() {
    if (peek() == 0xef && end_ - begin_ >= 3 &&
        static_cast<unsigned char>((*data_)[begin_+1]) == 0xbb &&
        static_cast<unsigned char>((*data_)[begin_+2]) == 0xbf) { get(); get(); get(); }
  }
private:
  int fd_;
  uint64_t position_ = 0;
  std::unique_ptr<std::array<char, 65536>> data_;
  size_t begin_ = 0, end_ = 0;
  bool failed_ = false;
};
Status csv_record(ByteReader &input, const Options &options, std::vector<Cell> &cells, Error &error)
{
  cells.clear();
  if (input.peek() < 0) return input.failed() ? error.fail(Status::INVALID, "file_sql: file read failed") : Status::END;
  size_t bytes = 0;
  bool done = false;
  while (!done) {
    if (cells.size() >= MAX_COLUMNS) return error.fail(Status::LIMIT, "file_sql: too many CSV columns");
    Cell cell;
    bool quoted = options.quote != '\0' && input.peek() == options.quote;
    if (quoted) input.get();
    bool closed = !quoted;
    while (true) {
      int c = input.get();
      if (++bytes > MAX_RECORD_SIZE) return error.fail(Status::LIMIT, "file_sql: CSV record exceeds size limit");
      if (c < 0) {
        if (quoted && !closed) return error.fail(Status::INVALID, "file_sql: unterminated quoted CSV field");
        done = true; break;
      }
      if (quoted && !closed) {
        if (c == options.escape && options.escape != options.quote) {
          c = input.get(); ++bytes;
          if (c < 0) return error.fail(Status::INVALID, "file_sql: incomplete CSV escape");
          cell.text.push_back(static_cast<char>(c));
        } else if (c == options.quote) {
          if (options.escape == options.quote && input.peek() == options.quote) {
            input.get(); ++bytes; cell.text.push_back(options.quote);
          } else closed = true;
        } else cell.text.push_back(static_cast<char>(c));
      } else if (c == options.delimiter) break;
      else if (c == '\n' || c == '\r') {
        if (c == '\r' && input.peek() == '\n') input.get();
        done = true; break;
      } else if (quoted || (options.quote != '\0' && c == options.quote)) {
        return error.fail(Status::INVALID, "file_sql: invalid bytes outside a quoted CSV field");
      } else cell.text.push_back(static_cast<char>(c));
    }
    if (!valid_utf8(cell.text)) return error.fail(Status::INVALID, "file_sql: CSV field is not valid UTF-8");
    cell.is_null = !quoted && cell.text == options.null_text;
    cells.push_back(std::move(cell));
  }
  return input.failed() ? error.fail(Status::INVALID, "file_sql: file read failed") : Status::OK;
}
void utf8_append(std::string &out, uint32_t cp)
{
  if (cp < 0x80) out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) { out.push_back(0xc0 | (cp >> 6)); out.push_back(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { out.push_back(0xe0 | (cp >> 12)); out.push_back(0x80 | ((cp >> 6) & 63)); out.push_back(0x80 | (cp & 63)); }
  else { out.push_back(0xf0 | (cp >> 18)); out.push_back(0x80 | ((cp >> 12) & 63)); out.push_back(0x80 | ((cp >> 6) & 63)); out.push_back(0x80 | (cp & 63)); }
}
class FlatJson {
public:
  explicit FlatJson(const std::string &text) : text_(text) {}
  bool parse(std::vector<std::pair<std::string, Cell>> &values) {
    values.clear(); std::set<std::string> keys;
    if (!take('{')) return false;
    if (take('}')) return end();
    do {
      std::string key; Cell value;
      if (!string(key) || !keys.insert(key).second || !take(':')) return false;
      spaces();
      if (position_ < text_.size() && text_[position_] == '"') {
        if (!string(value.text)) return false;
        value.is_null = false;
        value.source_type = infer_type(value.text);
        if (value.source_type != Type::DATE && value.source_type != Type::DATETIME)
          value.source_type = Type::VARCHAR;
      } else {
        size_t begin = position_;
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}' &&
               text_[position_] != ' ' && text_[position_] != '\t' && text_[position_] != '\r') ++position_;
        value.text = text_.substr(begin, position_ - begin);
        bool integer;
        if (value.text == "null") value.is_null = true;
        else if (value.text == "true" || value.text == "false" || json_number(value.text, integer)) value.is_null = false;
        else return false;
      }
      values.emplace_back(std::move(key), std::move(value));
      if (values.size() > MAX_COLUMNS) return false;
      if (take('}')) return end();
    } while (take(','));
    return false;
  }
private:
  static bool json_number(const std::string &value, bool &integer) {
    if (value.empty() || value[0] == '+') return false;
    size_t start = value[0] == '-' ? 1 : 0;
    if (start == value.size() || (value[start] == '0' && start+1 < value.size() && value[start+1] >= '0' && value[start+1] <= '9')) return false;
    if (!numeric_syntax(value, integer)) return false;
    size_t dot = value.find('.');
    return (dot == std::string::npos || (dot > start && dot+1 < value.size() && value[dot+1] >= '0' && value[dot+1] <= '9')) &&
           value[start] >= '0' && value[start] <= '9';
  }
  void spaces() { while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\r')) ++position_; }
  bool take(char c) { spaces(); if (position_ < text_.size() && text_[position_] == c) { ++position_; return true; } return false; }
  bool end() { spaces(); return position_ == text_.size(); }
  bool hex4(uint32_t &cp) {
    cp = 0;
    for (int i=0; i<4; ++i) {
      if (position_ == text_.size()) return false;
      char c=text_[position_++]; int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
      if (digit < 0) return false;
      cp = cp * 16 + digit;
    }
    return true;
  }
  bool string(std::string &out) {
    if (!take('"')) return false;
    out.clear();
    while (position_ < text_.size()) {
      unsigned char c=text_[position_++];
      if (c == '"') return valid_utf8(out);
      if (c < 32) return false;
      if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
      if (position_ == text_.size()) return false;
      c=text_[position_++];
      switch (c) {
        case '"': case '\\': case '/': out.push_back(static_cast<char>(c)); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp=0, low=0;
          if (!hex4(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (position_+2 > text_.size() || text_.compare(position_,2,"\\u") != 0) return false;
            position_ += 2;
            if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
            cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
          utf8_append(out,cp); break;
        }
        default: return false;
      }
    }
    return false;
  }
  const std::string &text_;
  size_t position_=0;
};
Status json_record(ByteReader &input, std::vector<std::pair<std::string, Cell>> &values, Error &error)
{
  std::string text;
  while (true) {
    int c=input.get();
    if (c < 0 && input.failed()) return error.fail(Status::INVALID,"file_sql: file read failed");
    if (c < 0 || c == '\n') {
      if (!trimmed(text).empty()) break;
      if (c < 0) return Status::END;
      text.clear(); continue;
    }
    text.push_back(static_cast<char>(c));
    if (text.size() > MAX_RECORD_SIZE) return error.fail(Status::LIMIT,"file_sql: JSONL record exceeds size limit");
  }
  if (!valid_utf8(text) || !FlatJson(text).parse(values))
    return error.fail(Status::INVALID,"file_sql: expected a valid flat JSON object (nested values are unsupported)");
  return Status::OK;
}
void unique_column_names(std::vector<Column> &columns)
{
  std::set<std::string> names;
  for (size_t i=0; i<columns.size(); ++i) {
    std::string base=columns[i].name.empty()?"column"+std::to_string(i):columns[i].name;
    std::string name=base;
    for (size_t suffix=2; !names.insert(ascii_lower(name)).second; ++suffix) name=base+"__"+std::to_string(suffix);
    columns[i].name=std::move(name);
  }
}
void observe(Column &column, const Cell &cell)
{
  if (cell.is_null) column.nullable=true;
  else {
    column.type=widen_type(column.type,cell.source_type == Type::NULL_VALUE ? infer_type(cell.text) : cell.source_type);
    column.max_length=std::max(column.max_length,static_cast<uint32_t>(cell.text.size()));
    int64_t temporal = 0;
    const std::string text = trimmed(cell.text);
    if (parse_temporal(text, true, temporal) && text.size() > 19) {
      column.fractional_scale = std::max(column.fractional_scale, static_cast<uint8_t>(text.size() - 20));
    }
  }
}
} // namespace

bool Fingerprint::operator==(const Fingerprint &b) const
{
  return device==b.device && inode==b.inode && size==b.size && mtime_sec==b.mtime_sec &&
      mtime_ns==b.mtime_ns && ctime_sec==b.ctime_sec && ctime_ns==b.ctime_ns;
}
const char *type_name(Type type)
{
  static const char *names[]={"NULL","BIGINT","DOUBLE","BOOLEAN","VARCHAR","DATE","DATETIME"};
  return static_cast<size_t>(type)<7?names[static_cast<size_t>(type)]:"INVALID";
}
bool valid_utf8(const std::string &text)
{
  for (size_t i=0; i<text.size();) {
    uint32_t c=static_cast<unsigned char>(text[i++]);
    if (c<0x80) continue;
    int count=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:-1;
    if (count<0 || i+count>text.size()) return false;
    uint32_t cp=c & ((1u<<(6-count))-1);
    for (int j=0;j<count;++j) { c=static_cast<unsigned char>(text[i++]); if ((c&0xc0)!=0x80) return false; cp=(cp<<6)|(c&63); }
    if ((count==1&&cp<0x80)||(count==2&&cp<0x800)||(count==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) return false;
  }
  return true;
}
bool parse_integer(const std::string &text, int64_t &value)
{
  std::string input=trimmed(text); bool integer;
  if (!numeric_syntax(input,integer)||!integer) return false;
  errno=0; char *end=nullptr; long long result=std::strtoll(input.c_str(),&end,10);
  if (errno==ERANGE||end!=input.c_str()+input.size()) return false;
  value=result; return true;
}
bool parse_boolean(const std::string &text, bool &value)
{
  std::string normalized=ascii_lower(trimmed(text));
  if(normalized!="true"&&normalized!="false")return false;
  value=normalized=="true";return true;
}
bool parse_double(const std::string &text, double &value)
{
  std::string input=trimmed(text); bool integer;
  if (!numeric_syntax(input,integer)) return false;
  errno=0; char *end=nullptr; double result=std::strtod(input.c_str(),&end);
  if (errno==ERANGE||end!=input.c_str()+input.size()||!std::isfinite(result)) return false;
  value=result; return true;
}
bool parse_temporal(const std::string &text, bool datetime, int64_t &value)
{
  std::string s=trimmed(text);
  if ((!datetime&&s.size()!=10)||(datetime&&s.size()<19)) return false;
  auto number=[&](size_t p,size_t n,int &v) {v=0;for(size_t i=p;i<p+n;++i){if(i>=s.size()||s[i]<'0'||s[i]>'9')return false;v=v*10+s[i]-'0';}return true;};
  int y,m,d,h=0,minute=0,second=0;
  if (!number(0,4,y)||!number(5,2,m)||!number(8,2,d)||s[4]!='-'||s[7]!='-'||y<1||m<1||m>12) return false;
  static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  int max_day=days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));
  if (d<1||d>max_day) return false;
  value=civil_days(y,m,d);
  if (!datetime) return true;
  if ((s[10]!='T'&&s[10]!=' ')||s[13]!=':'||s[16]!=':'||!number(11,2,h)||!number(14,2,minute)||!number(17,2,second)||h>23||minute>59||second>59) return false;
  int64_t fraction=0;
  if (s.size()>19) {
    if (s[19]!='.'||s.size()<21||s.size()>26) return false;
    for(size_t i=20;i<26;++i) {fraction*=10;if(i<s.size()){if(s[i]<'0'||s[i]>'9')return false;fraction+=s[i]-'0';}}
  }
  value=(value*86400+h*3600+minute*60+second)*1000000+fraction;
  return true;
}
Type infer_type(const std::string &text)
{
  std::string value=trimmed(text), lower=ascii_lower(value);
  if (lower=="true"||lower=="false") return Type::BOOLEAN;
  int64_t integer; double floating;
  if (parse_integer(value,integer)) return Type::BIGINT;
  bool integral;
  if (numeric_syntax(value,integral) && integral) return Type::VARCHAR;
  if (parse_double(value,floating)) return Type::DOUBLE;
  if (parse_temporal(value,false,integer)) return Type::DATE;
  if (parse_temporal(value,true,integer)) return Type::DATETIME;
  return Type::VARCHAR;
}
Type widen_type(Type a, Type b)
{
  if (a==Type::NULL_VALUE) return b;
  if (b==Type::NULL_VALUE||a==b) return a;
  auto numeric=[](Type t){return t==Type::BIGINT||t==Type::DOUBLE||t==Type::BOOLEAN;};
  if (numeric(a)&&numeric(b)) return a==Type::DOUBLE||b==Type::DOUBLE?Type::DOUBLE:Type::BIGINT;
  if ((a==Type::DATE&&b==Type::DATETIME)||(b==Type::DATE&&a==Type::DATETIME)) return Type::DATETIME;
  return Type::VARCHAR;
}
Status open_checked(const std::string &path,const std::string &root_input,int &fd,std::string &canonical,Fingerprint &fingerprint,Error &error)
{
  fd=-1;
  if(path.empty()||path.size()>4096||path.find('\0')!=std::string::npos||path.find("://")!=std::string::npos)
    return error.fail(Status::DENIED,"file_sql: expected a local file path");
  std::string root;
  Status status=canonical_root(root_input,root,error); if(status!=Status::OK)return status;
  char *resolved=::realpath(path.c_str(),nullptr);
  if(!resolved)return error.fail(errno==ENOENT?Status::MISSING:Status::DENIED,"file_sql: cannot resolve file "+path);
  canonical.assign(resolved);::free(resolved);
  if(!under_root(canonical,root))return error.fail(Status::DENIED,"file_sql: path escapes secure_file_priv");
  fd=open_components(canonical,O_RDONLY | O_NONBLOCK);
  if(fd<0)return error.fail(Status::DENIED,"file_sql: cannot securely open "+canonical);
  struct stat info;
  if(::fstat(fd,&info)!=0||!S_ISREG(info.st_mode)||info.st_size<0||static_cast<uint64_t>(info.st_size)>MAX_FILE_SIZE){
    ::close(fd);fd=-1;return error.fail(Status::LIMIT,"file_sql: expected a regular file within the size limit");
  }
  fingerprint=stamp(info); return Status::OK;
}

struct FileReader::Impl {
  int fd=-1;
  Definition definition;
  std::unique_ptr<ByteReader> input;
  std::unique_ptr<ParquetReader> parquet;
  std::vector<size_t> projection;
  std::map<std::string,size_t> column_map;
  std::vector<std::string> glob_paths;
  size_t glob_index=0;
  uint64_t record=0;
  ~Impl(){if(fd>=0)::close(fd);}
};
FileReader::FileReader()=default;
FileReader::~FileReader()=default;
void FileReader::close(){impl_.reset();}
Status FileReader::check_unchanged(Error &error) const
{
  if(!impl_||impl_->fd<0)return Status::OK;
  struct stat info;
  if(::fstat(impl_->fd,&info)!=0||!(stamp(info)==impl_->definition.fingerprint))
    return error.fail(Status::CHANGED,"file_sql: file changed after schema inference: "+impl_->definition.path);
  return Status::OK;
}
Status FileReader::open(const Definition &definition,const std::vector<size_t> &projection,Error &error)
{
  close();impl_.reset(new Impl);impl_->definition=definition;impl_->projection=projection;
  if(definition.format==Format::GLOB)return discover_files(definition.path,definition.secure_root,impl_->glob_paths,error);
  Fingerprint fingerprint;std::string canonical;
  Status status=open_checked(definition.path,definition.secure_root,impl_->fd,canonical,fingerprint,error);
  if(status!=Status::OK)return status;
  if(!(fingerprint==definition.fingerprint)){close();return error.fail(Status::CHANGED,"file_sql: file changed after schema inference");}
  if(definition.format==Format::PARQUET){
    impl_->parquet.reset(new ParquetReader);
    status=impl_->parquet->open(impl_->fd,definition,projection,error);
    if(status!=Status::OK)close();
    return status;
  }
  impl_->input.reset(new ByteReader(impl_->fd));impl_->input->skip_bom();
  for(size_t i=0;i<definition.columns.size();++i)impl_->column_map.emplace(definition.columns[i].name,i);
  if(definition.format==Format::CSV&&definition.options.header){std::vector<Cell> header;status=csv_record(*impl_->input,definition.options,header,error);if(status!=Status::OK)return status;}
  return Status::OK;
}
Status FileReader::rescan(Error &error)
{
  if(!impl_)return error.fail(Status::INVALID,"file_sql: rescan of a closed reader");
  Definition copy=impl_->definition;auto projection=impl_->projection;
  return open(copy,projection,error);
}
Status FileReader::next(std::vector<Cell> &row,Error &error)
{
  if(!impl_)return error.fail(Status::INVALID,"file_sql: reader is not open");
  Status status=check_unchanged(error);if(status!=Status::OK)return status;
  if(impl_->parquet)return impl_->parquet->next(row,error);
  if(impl_->definition.format==Format::GLOB){
    if(impl_->glob_index==impl_->glob_paths.size())return Status::END;
    row={{impl_->glob_paths[impl_->glob_index++],false}};return Status::OK;
  }
  if(!impl_->input)return error.fail(Status::INVALID,"file_sql: reader is not open");
  ++impl_->record;
  if(impl_->definition.format==Format::CSV){
    status=csv_record(*impl_->input,impl_->definition.options,row,error);
    if(status==Status::OK&&row.size()!=impl_->definition.columns.size())status=error.fail(Status::INVALID,"file_sql: CSV field count does not match header");
  }else{
    std::vector<std::pair<std::string,Cell>> fields;
    status=json_record(*impl_->input,fields,error);
    if(status==Status::OK){
      row.assign(impl_->definition.columns.size(),Cell{});
      for(auto &field:fields){auto found=impl_->column_map.find(field.first);if(found==impl_->column_map.end())return error.fail(Status::CHANGED,"file_sql: JSONL schema changed");row[found->second]=std::move(field.second);}
    }
  }
  if(status!=Status::OK&&status!=Status::END)error.message+=" at record "+std::to_string(impl_->record)+" in "+impl_->definition.path;
  return status;
}

Status FileReader::next_batch(size_t capacity, std::vector<std::vector<Cell>> &rows, Error &error)
{
  rows.clear();
  if (capacity == 0 || capacity > 1024) return error.fail(Status::INVALID, "file_sql: invalid batch capacity");
  size_t bytes = 0;
  try {
    while (rows.size() < capacity && bytes < MAX_RECORD_SIZE) {
      std::vector<Cell> row;
      Status status = next(row, error);
      if (status == Status::END) return rows.empty() ? Status::END : Status::OK;
      if (status != Status::OK) { rows.clear(); return status; }
      bytes += row.capacity() * sizeof(Cell);
      for (const Cell &cell : row) bytes += cell.text.capacity();
      rows.push_back(std::move(row));
    }
    return Status::OK;
  } catch (const std::bad_alloc &) {
    rows.clear();
    return error.fail(Status::LIMIT, "file_sql: batch allocation failed");
  }
}

Status infer_schema(Definition &definition,Error &error)
{
  try {
    definition.columns.clear();definition.rows=0;
    if(definition.format==Format::GLOB){definition.columns.push_back({"file",Type::VARCHAR,false,4096});return Status::OK;}
    int descriptor=-1;std::string canonical;Fingerprint fingerprint;
    Status status=open_checked(definition.path,definition.secure_root,descriptor,canonical,fingerprint,error);
    if(status!=Status::OK)return status;
    struct Close {int fd;~Close(){::close(fd);}} guard{descriptor};
    definition.path=canonical;definition.fingerprint=fingerprint;
    if(definition.format==Format::PARQUET){
      status=infer_parquet_schema(descriptor,definition,error);
      struct stat latest;
      if(status==Status::OK&&(::fstat(descriptor,&latest)!=0||!(stamp(latest)==fingerprint)))
        return error.fail(Status::CHANGED,"file_sql: Parquet file changed during inference");
      return status;
    }
    ByteReader input(descriptor);input.skip_bom();
    if(definition.format==Format::CSV){
      std::vector<Cell> row;
      status=csv_record(input,definition.options,row,error);
      if(status==Status::END)return error.fail(Status::INVALID,"file_sql: cannot infer an empty CSV without a header");
      if(status!=Status::OK)return status;
      for(size_t i=0;i<row.size();++i)definition.columns.push_back({definition.options.header?row[i].text:"column"+std::to_string(i),Type::NULL_VALUE,false,0});
      unique_column_names(definition.columns);
      if(!definition.options.header){for(size_t i=0;i<row.size();++i)observe(definition.columns[i],row[i]);++definition.rows;}
      while((status=csv_record(input,definition.options,row,error))==Status::OK){
        if(row.size()!=definition.columns.size())return error.fail(Status::INVALID,"file_sql: CSV field count mismatch at record "+std::to_string(definition.rows+1));
        for(size_t i=0;i<row.size();++i)observe(definition.columns[i],row[i]);++definition.rows;
      }
    }else{
      std::map<std::string,size_t> map;
      std::vector<std::pair<std::string,Cell>> row;
      while((status=json_record(input,row,error))==Status::OK){
        std::vector<bool> present(definition.columns.size(),false);
        for(const auto &field:row){
          auto found=map.find(field.first);size_t index;
          if(found==map.end()){
            if(definition.columns.size()>=MAX_COLUMNS)return error.fail(Status::LIMIT,"file_sql: too many JSONL columns");
            index=definition.columns.size();map[field.first]=index;
            definition.columns.push_back({field.first,Type::NULL_VALUE,definition.rows!=0,0});present.push_back(false);
          }else index=found->second;
          present[index]=true;observe(definition.columns[index],field.second);
        }
        for(size_t i=0;i<present.size();++i)if(!present[i])definition.columns[i].nullable=true;
        ++definition.rows;
      }
      if(definition.columns.empty())return error.fail(Status::INVALID,"file_sql: cannot infer columns from empty JSONL objects");
      std::set<std::string> names;
      for(const Column &column:definition.columns)if(column.name.empty()||!names.insert(ascii_lower(column.name)).second)return error.fail(Status::INVALID,"file_sql: ambiguous JSONL column names");
    }
    if(status!=Status::END){error.message+=" at record "+std::to_string(definition.rows+1)+" in "+definition.path;return status;}
    for(Column &column:definition.columns)if(column.type==Type::NULL_VALUE)column.nullable=true;
    struct stat final_stat;
    if(::fstat(descriptor,&final_stat)!=0||!(stamp(final_stat)==fingerprint))return error.fail(Status::CHANGED,"file_sql: file changed during schema inference");
    return Status::OK;
  }catch(const std::bad_alloc &){return error.fail(Status::LIMIT,"file_sql: schema inference memory limit");}
}

Status discover_files(const std::string &pattern,const std::string &secure_root,std::vector<std::string> &paths,Error &error)
{
  paths.clear();std::string root;
  Status status=canonical_root(secure_root,root,error);if(status!=Status::OK)return status;
  if(pattern.empty()||pattern.find('\0')!=std::string::npos||pattern.find("://")!=std::string::npos)return error.fail(Status::DENIED,"file_sql: invalid glob pattern");
  size_t wildcard=pattern.find_first_of("*?[");
  size_t slash=pattern.rfind('/',wildcard);
  std::string prefix=slash==std::string::npos?".":slash==0?"/":pattern.substr(0,slash);
  char *resolved=::realpath(prefix.c_str(),nullptr);
  if(!resolved)return error.fail(Status::DENIED,"file_sql: glob base directory does not exist");
  std::string canonical(resolved);::free(resolved);
  if(!under_root(canonical,root))return error.fail(Status::DENIED,"file_sql: glob escapes secure_file_priv");
  glob_t matches{};
  int code=::glob(pattern.c_str(),GLOB_NOSORT,nullptr,&matches);
  struct Release{glob_t *p;~Release(){::globfree(p);}} release{&matches};
  if(code==GLOB_NOMATCH)return Status::OK;
  if(code!=0||matches.gl_pathc>MAX_GLOB_FILES)return error.fail(Status::LIMIT,"file_sql: glob discovery exceeds limit");
  for(size_t i=0;i<matches.gl_pathc;++i){
    struct stat info;
    if(::stat(matches.gl_pathv[i],&info)!=0)return error.fail(Status::INVALID,"file_sql: glob entry changed");
    if(!S_ISREG(info.st_mode))continue;
    int fd=-1;Fingerprint fingerprint;std::string actual;
    status=open_checked(matches.gl_pathv[i],secure_root,fd,actual,fingerprint,error);
    if(status!=Status::OK)return status;
    ::close(fd);paths.push_back(std::move(actual));
  }
  std::sort(paths.begin(),paths.end());return Status::OK;
}

namespace {
void put64(std::string &out,uint64_t value){for(int i=0;i<8;++i)out.push_back(static_cast<char>(value>>(i*8)));}
void put_string(std::string &out,const std::string &text){put64(out,text.size());out.append(text);}
class Decoder {
public:
  explicit Decoder(const std::string &data):data_(data){}
  bool number(uint64_t &v){if(pos_+8>data_.size())return false;v=0;for(int i=0;i<8;++i)v|=uint64_t(static_cast<unsigned char>(data_[pos_++]))<<(i*8);return true;}
  bool string(std::string &s,size_t limit){uint64_t n;if(!number(n)||n>limit||n>data_.size()-pos_)return false;s.assign(data_,pos_,n);pos_+=n;return true;}
  bool done()const{return pos_==data_.size();}
private:const std::string &data_;size_t pos_=0;
};
}
Status encode_definition(const Definition &d,std::string &out,Error &error)
{
  try{
    out.clear();put_string(out,"seekdb-file-table-v2");put64(out,static_cast<uint64_t>(d.format));
    put64(out,d.options.header);put64(out,static_cast<unsigned char>(d.options.delimiter));put64(out,static_cast<unsigned char>(d.options.quote));put64(out,static_cast<unsigned char>(d.options.escape));
    put_string(out,d.options.null_text);put_string(out,d.path);put_string(out,d.secure_root);
    const auto &f=d.fingerprint;
    for(uint64_t v:{f.device,f.inode,f.size,static_cast<uint64_t>(f.mtime_sec),static_cast<uint64_t>(f.mtime_ns),static_cast<uint64_t>(f.ctime_sec),static_cast<uint64_t>(f.ctime_ns),d.rows,uint64_t(d.columns.size())})put64(out,v);
    for(const Column &c:d.columns){put_string(out,c.name);put64(out,static_cast<uint64_t>(c.type));put64(out,c.nullable);put64(out,c.max_length);put64(out,c.fractional_scale);}
    if(out.size()>4*1024*1024)return error.fail(Status::LIMIT,"file_sql: schema metadata exceeds limit");
    return Status::OK;
  }catch(const std::bad_alloc &){return error.fail(Status::LIMIT,"file_sql: metadata memory limit");}
}
Status decode_definition(const std::string &encoded,Definition &d,Error &error)
{
  Decoder decoder(encoded);std::string version;uint64_t value;
  auto invalid=[&](){return error.fail(Status::INVALID,"file_sql: corrupt file table metadata");};
  if(encoded.size()>4*1024*1024||!decoder.string(version,64)||version!="seekdb-file-table-v2")return invalid();
  if(!decoder.number(value)||value>3)return invalid();d.format=static_cast<Format>(value);
  if(!decoder.number(value)||value>1)return invalid();d.options.header=value;
  if(!decoder.number(value)||value>255)return invalid();d.options.delimiter=value;
  if(!decoder.number(value)||value>255)return invalid();d.options.quote=value;
  if(!decoder.number(value)||value>255)return invalid();d.options.escape=value;
  if(!decoder.string(d.options.null_text,MAX_RECORD_SIZE)||!decoder.string(d.path,4096)||!decoder.string(d.secure_root,4096))return invalid();
  auto &f=d.fingerprint;
  if(!decoder.number(f.device)||!decoder.number(f.inode)||!decoder.number(f.size))return invalid();
  if(!decoder.number(value))return invalid();f.mtime_sec=value;
  if(!decoder.number(value))return invalid();f.mtime_ns=value;
  if(!decoder.number(value))return invalid();f.ctime_sec=value;
  if(!decoder.number(value))return invalid();f.ctime_ns=value;
  uint64_t count;
  if(!decoder.number(d.rows)||!decoder.number(count)||count>MAX_COLUMNS)return invalid();d.columns.clear();
  for(uint64_t i=0;i<count;++i){Column c;if(!decoder.string(c.name,4096)||!decoder.number(value)||value>6)return invalid();c.type=static_cast<Type>(value);if(!decoder.number(value)||value>1)return invalid();c.nullable=value;if(!decoder.number(value)||value>MAX_RECORD_SIZE)return invalid();c.max_length=value;if(!decoder.number(value)||value>6)return invalid();c.fractional_scale=value;d.columns.push_back(std::move(c));}
  return decoder.done()?Status::OK:invalid();
}

struct CsvWriter::Impl {
  int parent=-1,fd=-1;
  Options options;
  std::string target,temporary;
  uint64_t bytes=0;
  ~Impl(){
    if(fd>=0)::close(fd);
    if(parent>=0){if(!temporary.empty())::unlinkat(parent,temporary.c_str(),0);::close(parent);}
  }
};
CsvWriter::CsvWriter()=default;
CsvWriter::~CsvWriter()=default;
void CsvWriter::close(){impl_.reset();}
Status CsvWriter::open(const Definition &definition,Error &error)
{
  close();
  const std::string &path=definition.path;
  if(path.empty()||path.size()>4096||path.find('\0')!=std::string::npos||path.find("://")!=std::string::npos)
    return error.fail(Status::DENIED,"file_sql: COPY requires a local output file");
  const Options &options=definition.options;
  if(options.delimiter==0||options.delimiter=='\n'||options.delimiter=='\r'||
     (options.quote!=0&&options.quote==options.delimiter)||
     options.null_text.find_first_of(std::string(1,options.delimiter)+"\r\n")!=std::string::npos)
    return error.fail(Status::INVALID,"file_sql: invalid COPY CSV options");
  size_t slash=path.rfind('/');
  std::string directory=slash==std::string::npos?".":slash==0?"/":path.substr(0,slash);
  std::string target=slash==std::string::npos?path:path.substr(slash+1);
  if(target.empty()||target=="."||target=="..")return error.fail(Status::DENIED,"file_sql: invalid COPY target");
  std::string root;Status status=canonical_root(definition.secure_root,root,error);if(status!=Status::OK)return status;
  char *resolved=::realpath(directory.c_str(),nullptr);
  if(!resolved)return error.fail(Status::MISSING,"file_sql: COPY parent directory does not exist");
  directory.assign(resolved);::free(resolved);
  if(!under_root(directory,root))return error.fail(Status::DENIED,"file_sql: COPY target escapes secure_file_priv");
  impl_.reset(new Impl);impl_->options=options;impl_->target=target;
  impl_->parent=open_components(directory,O_RDONLY|O_DIRECTORY);
  if(impl_->parent<0){close();return error.fail(Status::DENIED,"file_sql: cannot securely open COPY directory");}
  struct stat existing;
  if(::fstatat(impl_->parent,target.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT){close();return error.fail(Status::INVALID,"file_sql: COPY target already exists or cannot be checked");}
  static std::atomic<uint64_t> sequence{0};
  for(int attempt=0;attempt<64;++attempt){
    impl_->temporary=".seekdb-copy-"+std::to_string(::getpid())+"-"+std::to_string(sequence.fetch_add(1));
    impl_->fd=::openat(impl_->parent,impl_->temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(impl_->fd>=0)break;
    impl_->temporary.clear();
    if(errno!=EEXIST)break;
  }
  if(impl_->fd<0){close();return error.fail(Status::DENIED,"file_sql: cannot create COPY temporary file");}
  if(options.header){
    std::vector<Cell> header;for(const Column &column:definition.columns)header.push_back({column.name,false});
    status=append(header,error);if(status!=Status::OK)close();
  }
  return status;
}
Status CsvWriter::append(const std::vector<Cell> &row,Error &error)
{
  if(!impl_||impl_->fd<0)return error.fail(Status::INVALID,"file_sql: COPY writer is not open");
  const Options &o=impl_->options;std::string record;
  for(size_t i=0;i<row.size();++i){
    if(i)record.push_back(o.delimiter);
    const Cell &cell=row[i];
    if(cell.is_null){record+=o.null_text;continue;}
    if(!valid_utf8(cell.text))return error.fail(Status::INVALID,"file_sql: COPY requires UTF-8 text");
    bool quote=cell.text.empty()||cell.text==o.null_text||cell.text.find(o.delimiter)!=std::string::npos||
               cell.text.find_first_of("\r\n")!=std::string::npos||(o.quote!=0&&cell.text.find(o.quote)!=std::string::npos);
    if(quote&&o.quote==0)return error.fail(Status::INVALID,"file_sql: COPY field requires quoting");
    if(quote)record.push_back(o.quote);
    for(char c:cell.text){
      if(quote&&(c==o.quote||(o.escape!=o.quote&&o.escape!=0&&c==o.escape))){
        if(o.escape==0)return error.fail(Status::INVALID,"file_sql: COPY field requires escaping");
        record.push_back(o.escape);
      }
      record.push_back(c);
      if(record.size()>MAX_RECORD_SIZE)return error.fail(Status::LIMIT,"file_sql: COPY record exceeds size limit");
    }
    if(quote)record.push_back(o.quote);
  }
  record.push_back('\n');
  if(record.size()>MAX_RECORD_SIZE||impl_->bytes+record.size()>MAX_FILE_SIZE)return error.fail(Status::LIMIT,"file_sql: COPY output exceeds size limit");
  size_t offset=0;
  while(offset<record.size()){
    ssize_t size=::write(impl_->fd,record.data()+offset,record.size()-offset);
    if(size<0&&errno==EINTR)continue;
    if(size<=0)return error.fail(Status::INVALID,"file_sql: COPY write failed");
    offset+=size;
  }
  impl_->bytes+=record.size();return Status::OK;
}
Status CsvWriter::finish(Error &error)
{
  if(!impl_||impl_->fd<0)return error.fail(Status::INVALID,"file_sql: COPY writer is not open");
  if(::fsync(impl_->fd)!=0){close();return error.fail(Status::INVALID,"file_sql: COPY flush failed");}
  int descriptor=impl_->fd;impl_->fd=-1;
  if(::close(descriptor)!=0){close();return error.fail(Status::INVALID,"file_sql: COPY close failed");}
  // A hard link publishes the completed file atomically and never replaces an existing target.
  if(::linkat(impl_->parent,impl_->temporary.c_str(),impl_->parent,impl_->target.c_str(),0)!=0){close();return error.fail(Status::INVALID,"file_sql: COPY target exists or publication failed");}
  ::unlinkat(impl_->parent,impl_->temporary.c_str(),0);impl_->temporary.clear();close();return Status::OK;
}

}}} // namespace oceanbase::sql::filesql
