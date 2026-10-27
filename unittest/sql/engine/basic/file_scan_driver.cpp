#include "sql/engine/basic/ob_file_scan_utils.h"
#include <iostream>
using namespace oceanbase::sql::filesql;
static void string(const std::string &s)
{
  std::cout<<'"';const char *hex="0123456789abcdef";
  for(unsigned char c:s){if(c=='"'||c=='\\')std::cout<<'\\'<<c;else if(c<32)std::cout<<"\\u00"<<hex[c>>4]<<hex[c&15];else std::cout<<c;}
  std::cout<<'"';
}
int main(int argc,char **argv)
{
  if(argc<2)return 2;Definition d;d.path=argv[1];d.secure_root="/";d.format=Format::PARQUET;Error e;
  if(infer_schema(d,e)!=Status::OK){std::cerr<<e.message<<'\n';return 1;}
  std::vector<size_t> projection;
  if(argc>2){std::string p=argv[2];for(char c:p)if(c>='0'&&c<='9')projection.push_back(c-'0');}
  FileReader reader;if(reader.open(d,projection,e)!=Status::OK){std::cerr<<e.message<<'\n';return 1;}
  std::cout<<"{\"schema\":[";
  for(size_t i=0;i<d.columns.size();++i){if(i)std::cout<<',';string(d.columns[i].name);}
  std::cout<<"],\"rows\":[";std::vector<Cell> row;bool first=true;Status status;
  while((status=reader.next(row,e))==Status::OK){
    if(!first)std::cout<<',';first=false;std::cout<<'[';
    for(size_t i=0;i<row.size();++i){if(i)std::cout<<',';if(row[i].is_null)std::cout<<"null";else string(row[i].text);}
    std::cout<<']';
  }
  if(status!=Status::END){std::cerr<<e.message<<'\n';return 1;}
  std::cout<<"]}\n";
}
