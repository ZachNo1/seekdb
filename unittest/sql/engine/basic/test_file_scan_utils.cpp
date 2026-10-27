#include "sql/engine/basic/ob_file_scan_utils.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace oceanbase::sql::filesql;

static void write_file(const std::string &path,const std::string &text)
{
  std::ofstream out(path,std::ios::binary);out<<text;assert(out.good());
}
int main()
{
  char temporary[]="/tmp/seekdb-file-unit-XXXXXX";
  char *directory=::mkdtemp(temporary);assert(directory);
  std::string root=directory;Error error;
  Definition d;d.path=root+"/test.csv";d.secure_root=root;
  write_file(d.path,"id,id,note,flag,date,nothing\r\n1,2,\"hello,\nworld\",true,2024-02-29,\r\n3,4,\"say \"\"hi\"\"\",false,,,\n");
  assert(infer_schema(d,error)==Status::INVALID); // bad field count
  write_file(d.path,"id,id,note,flag,date,nothing\r\n1,2,\"hello,\nworld\",true,2024-02-29,\r\n3,4,\"say \"\"hi\"\"\",false,,\n");
  assert(infer_schema(d,error)==Status::OK);
  assert(d.rows==2&&d.columns.size()==6&&d.columns[1].name=="id__2");
  assert(d.columns[0].type==Type::BIGINT&&d.columns[3].type==Type::BOOLEAN);
  assert(d.columns[4].type==Type::DATE&&d.columns[4].nullable);
  assert(d.columns[5].type==Type::NULL_VALUE&&d.columns[5].nullable);
  std::string packed;assert(encode_definition(d,packed,error)==Status::OK);
  Definition decoded;assert(decode_definition(packed,decoded,error)==Status::OK);
  assert(decoded.fingerprint==d.fingerprint&&decoded.columns[1].name=="id__2");
  FileReader reader;std::vector<Cell> row;
  assert(reader.open(decoded,{},error)==Status::OK);
  assert(reader.next(row,error)==Status::OK&&row[2].text=="hello,\nworld");
  assert(reader.next(row,error)==Status::OK&&row[2].text=="say \"hi\"");
  assert(reader.next(row,error)==Status::END);
  assert(reader.rescan(error)==Status::OK&&reader.next(row,error)==Status::OK);
  std::vector<std::vector<Cell>> batch;
  assert(reader.rescan(error)==Status::OK);
  assert(reader.next_batch(128,batch,error)==Status::OK&&batch.size()==2);
  assert(batch[0][2].text=="hello,\nworld"&&batch[1][2].text=="say \"hi\"");
  assert(reader.next_batch(128,batch,error)==Status::END&&batch.empty());
  assert(reader.next_batch(0,batch,error)==Status::INVALID);
  write_file(d.path,"changed\n");
  assert(reader.next(row,error)==Status::CHANGED);
  assert(reader.open(decoded,{},error)==Status::CHANGED);
  reader.close();
  d.format=Format::JSONL;d.path=root+"/test.jsonl";
  write_file(d.path,"{\"id\":1,\"name\":\"\\u4f60\\u597d\\ud83d\\ude00\",\"allnull\":null}\n{\"id\":2.5,\"extra\":true}\n");
  assert(infer_schema(d,error)==Status::OK);
  assert(d.columns.size()==4&&d.columns[0].type==Type::DOUBLE);
  assert(d.columns[1].nullable&&d.columns[2].type==Type::NULL_VALUE&&d.columns[3].nullable);
  assert(reader.open(d,{},error)==Status::OK&&reader.next(row,error)==Status::OK);
  assert(row[1].text=="你好😀"&&row[3].is_null);
  assert(reader.next(row,error)==Status::OK&&row[1].is_null&&row[3].text=="true");
  reader.close();
  write_file(d.path,"{\"v\":1}\n{\"v\":\"2\"}\n");
  assert(infer_schema(d,error)==Status::OK&&d.columns[0].type==Type::VARCHAR);
  for(const std::string &bad:{"{\"v\":[1]}\n","{\"v\":{}}\n","{\"v\":01}\n","{\"v\":\"\\ud800\"}\n","{\"v\":1,\"v\":2}\n"}){
    write_file(d.path,bad);assert(infer_schema(d,error)==Status::INVALID);
  }

  d.format=Format::CSV;d.path=root+"/precision.csv";
  write_file(d.path,"dt\n2024-02-29 00:00:00\n2024-02-29 00:00:00.000001\n");
  assert(infer_schema(d,error)==Status::OK&&d.columns[0].fractional_scale==6);
  assert(encode_definition(d,packed,error)==Status::OK);
  assert(decode_definition(packed,decoded,error)==Status::OK&&decoded.columns[0].fractional_scale==6);
  ::unlink(d.path.c_str());
  Definition output;output.path=root+"/roundtrip.csv";output.secure_root=root;
  output.options.header=true;output.options.delimiter=';';output.options.escape='\\';output.options.null_text="NULL";
  output.columns={{"empty",Type::VARCHAR},{"literal",Type::VARCHAR},{"null",Type::NULL_VALUE},{"quoted",Type::VARCHAR}};
  CsvWriter writer;assert(writer.open(output,error)==Status::OK);
  assert(writer.append({{"",false},{"NULL",false},{"",true},{"a;\"b\\c\n你好",false}},error)==Status::OK);
  assert(writer.finish(error)==Status::OK);
  assert(writer.open(output,error)==Status::INVALID);
  assert(infer_schema(output,error)==Status::OK&&reader.open(output,{},error)==Status::OK);
  assert(reader.next(row,error)==Status::OK&&row.size()==4&&!row[0].is_null&&row[0].text.empty());
  assert(!row[1].is_null&&row[1].text=="NULL"&&row[2].is_null&&row[3].text=="a;\"b\\c\n你好");
  reader.close();::unlink(output.path.c_str());
  output.path=root+"/abort.csv";output.options.quote=0;output.options.header=false;
  assert(writer.open(output,error)==Status::OK);
  assert(writer.append({{"needs;quotes",false}},error)==Status::INVALID);
  writer.close();assert(::access(output.path.c_str(),F_OK)!=0);
  d.path=root+"/test.jsonl";

  int64_t integer;double number;bool boolean;
  assert(parse_integer("-9223372036854775808",integer)&&integer==INT64_MIN);
  assert(infer_type("9223372036854775808")==Type::VARCHAR);
  assert(infer_type("1e309")==Type::VARCHAR&&!parse_double("NaN",number));
  assert(parse_boolean("FALSE",boolean)&&!boolean);
  assert(parse_temporal("2024-02-29",false,integer)&&!parse_temporal("2023-02-29",false,integer));
  assert(parse_temporal("1970-01-01T00:00:00.000001",true,integer)&&integer==1);
  assert(!parse_temporal("2024-01-01 24:00:00",true,integer));
  int fd=-1;Fingerprint fingerprint;std::string canonical;
  assert(open_checked("/etc/passwd",root,fd,canonical,fingerprint,error)==Status::DENIED);
  std::string link=root+"/escape";assert(::symlink("/etc/passwd",link.c_str())==0);
  assert(open_checked(link,root,fd,canonical,fingerprint,error)==Status::DENIED);
  std::string fifo=root+"/pipe";assert(::mkfifo(fifo.c_str(),0600)==0);
  assert(open_checked(fifo,root,fd,canonical,fingerprint,error)==Status::LIMIT);
  assert(open_checked(d.path,"NULL",fd,canonical,fingerprint,error)==Status::DENIED);
  ::unlink(link.c_str());::unlink(fifo.c_str());
  std::vector<std::string> paths;
  assert(discover_files(root+"/*",root,paths,error)==Status::OK&&paths.size()==2);
  assert(discover_files("/etc/*",root,paths,error)==Status::DENIED);
  ::unlink((root+"/test.csv").c_str());::unlink(d.path.c_str());::rmdir(root.c_str());
  std::cout<<"file scan utilities: CSV, JSONL, inference, rescan, fingerprints and secure paths passed\n";
}
