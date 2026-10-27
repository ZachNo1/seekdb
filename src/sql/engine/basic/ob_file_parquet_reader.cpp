/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "ob_file_parquet_reader.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase { namespace sql { namespace filesql {
namespace parquet_detail {
constexpr size_t MAX_METADATA = 16 * 1024 * 1024;
constexpr size_t MAX_PAGE = 32 * 1024 * 1024;
constexpr size_t MAX_VALUES = 262144;
constexpr size_t MAX_LIVE_PAGES = 128 * 1024 * 1024;

uint32_t le32(const char *data)
{
  uint32_t value=0;for(int i=0;i<4;++i)value|=uint32_t(static_cast<unsigned char>(data[i]))<<(8*i);return value;
}
uint64_t le64(const char *data)
{
  uint64_t value=0;for(int i=0;i<8;++i)value|=uint64_t(static_cast<unsigned char>(data[i]))<<(8*i);return value;
}
bool read_at(int fd,uint64_t offset,size_t size,std::string &out)
{
  out.resize(size);size_t done=0;
  while(done<size){ssize_t n=::pread(fd,&out[done],size-done,offset+done);if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=n;}
  return true;
}
struct Node {
  uint8_t type=0;
  int64_t integer=0;
  std::string text;
  std::vector<Node> list;
  std::map<int,Node> fields;
  const Node *field(int id)const{auto it=fields.find(id);return it==fields.end()?nullptr:&it->second;}
  int64_t number(int id,int64_t fallback=0)const{const Node *n=field(id);return n?n->integer:fallback;}
};
class Compact {
public:
  explicit Compact(const std::string &data):data_(data){}
  bool root(Node &out){return value(12,out,0,false);}
  size_t position()const{return position_;}
private:
  bool byte(uint8_t &out){if(position_>=data_.size())return false;out=static_cast<unsigned char>(data_[position_++]);return true;}
  bool varint(uint64_t &out){out=0;for(int i=0;i<10;++i){uint8_t b;if(!byte(b)||(i==9&&b>1))return false;out|=uint64_t(b&127)<<(7*i);if(!(b&128))return true;}return false;}
  bool value(uint8_t type,Node &out,size_t depth,bool collection){
    if(depth>64||++nodes_>262144)return false;out.type=type;uint64_t number=0;uint8_t b=0;
    switch(type){
      case 1:case 2:
        if(collection){if(!byte(b)||(b!=1&&b!=2))return false;out.integer=b==1;}
        else out.integer=type==1;return true;
      case 3:if(!byte(b))return false;out.integer=static_cast<int8_t>(b);return true;
      case 4:case 5:case 6:
        if(!varint(number))return false;out.integer=static_cast<int64_t>((number>>1)^uint64_t(-static_cast<int64_t>(number&1)));return true;
      case 7:if(position_+8>data_.size())return false;position_+=8;return true;
      case 8:
        if(!varint(number)||number>MAX_METADATA||number>data_.size()-position_)return false;
        out.text.assign(data_,position_,number);position_+=number;return true;
      case 9:case 10:{
        if(!byte(b))return false;uint64_t size=b>>4;uint8_t element=b&15;
        if(size==15&&!varint(size))return false;
        // Reserve the allocation budget before resize, not only when visiting
        // children: malicious nested lists can otherwise allocate repeatedly.
        if(size>262144-allocated_nodes_)return false;allocated_nodes_+=size;
        out.list.resize(size);for(Node &node:out.list)if(!value(element,node,depth+1,true))return false;return true;
      }
      case 11:{
        if(!varint(number)||number>65536)return false;if(number==0)return true;if(!byte(b))return false;
        for(uint64_t i=0;i<number;++i){Node key,item;if(!value(b>>4,key,depth+1,true)||!value(b&15,item,depth+1,true))return false;}
        return true;
      }
      case 12:{
        int previous=0;
        while(true){
          if(!byte(b))return false;if(b==0)return true;
          int id=b>>4;uint8_t kind=b&15;
          if(id!=0)id+=previous;else{if(!varint(number))return false;id=static_cast<int64_t>((number>>1)^uint64_t(-static_cast<int64_t>(number&1)));}
          if(id<=0||id>32767||out.fields.count(id))return false;previous=id;
          if(allocated_nodes_>=262144)return false;++allocated_nodes_;
          Node child;if(!value(kind,child,depth+1,false))return false;out.fields.emplace(id,std::move(child));
        }
      }
      default:return false;
    }
  }
  const std::string &data_;size_t position_=0,nodes_=0,allocated_nodes_=0;
};
struct PhysicalColumn {
  std::string name;
  int physical=0;
  int converted=-1;
  int unit=0; // 1 milliseconds, 2 microseconds, 3 nanoseconds
  int fixed_length=0;
  bool optional=false;
  Type type=Type::VARCHAR;
};
struct Chunk {int codec=0;uint64_t offset=0,end=0;int64_t values=0;};
struct Group {uint64_t rows=0;std::vector<Chunk> chunks;};
struct Footer {std::vector<PhysicalColumn> columns;std::vector<Group> groups;uint64_t rows=0,footer_offset=0;};

Status parse_footer(int fd,Footer &footer,Error &error)
{
  struct stat info;std::string magic,tail,data;
  if(::fstat(fd,&info)!=0||info.st_size<12||!read_at(fd,0,4,magic)||magic!="PAR1"||
     !read_at(fd,info.st_size-8,8,tail)||tail.compare(4,4,"PAR1")!=0)
    return error.fail(Status::INVALID,"file_sql: invalid Parquet magic/footer");
  uint32_t length=le32(tail.data());
  if(length>MAX_METADATA||uint64_t(length)+12>uint64_t(info.st_size))return error.fail(Status::LIMIT,"file_sql: invalid Parquet footer size");
  footer.footer_offset=info.st_size-8-length;
  if(!read_at(fd,footer.footer_offset,length,data))return error.fail(Status::INVALID,"file_sql: truncated Parquet footer");
  Node root;Compact parser(data);
  if(!parser.root(root)||parser.position()!=data.size())return error.fail(Status::INVALID,"file_sql: corrupt Parquet metadata");
  const Node *schema=root.field(2),*groups=root.field(4);
  if(!schema||schema->type!=9||schema->list.empty()||!groups||groups->type!=9||root.number(3,-1)<0)
    return error.fail(Status::INVALID,"file_sql: missing Parquet schema/row groups");
  const int64_t count=schema->list[0].number(5,-1);
  if(count<=0||count>MAX_COLUMNS||schema->list.size()!=size_t(count+1))return error.fail(Status::UNSUPPORTED,"file_sql: only flat Parquet schemas are supported");
  footer.columns.clear();footer.groups.clear();footer.rows=0;std::set<std::string> names;
  for(int64_t i=0;i<count;++i){
    const Node &item=schema->list[i+1];const Node *name=item.field(4);
    if(!name||name->type!=8||name->text.empty()||!valid_utf8(name->text)||!names.insert(name->text).second||!item.field(1)||item.number(5)!=0)
      return error.fail(Status::INVALID,"file_sql: invalid Parquet leaf column");
    PhysicalColumn column;column.name=name->text;column.physical=item.number(1);column.converted=item.number(6,-1);column.fixed_length=item.number(2);
    int repetition=item.number(3,0);if(repetition!=0&&repetition!=1)return error.fail(Status::UNSUPPORTED,"file_sql: repeated Parquet fields are unsupported");
    column.optional=repetition==1;
    switch(column.physical){
      case 0:column.type=Type::BOOLEAN;break;
      case 1:case 2:column.type=Type::BIGINT;break;
      case 3:column.type=Type::DATETIME;break;
      case 4:case 5:column.type=Type::DOUBLE;break;
      case 6:case 7:column.type=Type::VARCHAR;break;
      default:return error.fail(Status::UNSUPPORTED,"file_sql: unsupported Parquet physical type");
    }
    if(column.converted==6)column.type=Type::DATE;
    else if(column.converted==9||column.converted==10){column.type=Type::DATETIME;column.unit=column.converted==9?1:2;}
    else if(column.converted==14)column.type=Type::VARCHAR; // unsigned 64-bit values need lossless text
    else if(column.converted==5)return error.fail(Status::UNSUPPORTED,"file_sql: Parquet DECIMAL is unsupported");
    const Node *logical=item.field(10);
    if(logical){
      if(logical->field(6))column.type=Type::DATE;
      if(const Node *timestamp=logical->field(8)){
        const Node *unit=timestamp->field(2);
        if(!unit)return error.fail(Status::INVALID,"file_sql: invalid Parquet timestamp unit");
        column.type=Type::DATETIME;column.unit=unit->field(1)?1:unit->field(2)?2:unit->field(3)?3:0;
        if(!column.unit)return error.fail(Status::UNSUPPORTED,"file_sql: unsupported Parquet timestamp unit");
      }
    }
    footer.columns.push_back(std::move(column));
  }
  if(groups->list.size()>65536)return error.fail(Status::LIMIT,"file_sql: too many Parquet row groups");
  for(const Node &item:groups->list){
    Group group;int64_t rows=item.number(3,-1);const Node *chunks=item.field(1);
    if(rows<0||!chunks||chunks->type!=9||chunks->list.size()!=size_t(count))return error.fail(Status::INVALID,"file_sql: invalid Parquet row group");
    group.rows=rows;
    for(size_t i=0;i<chunks->list.size();++i){
      const Node *meta=chunks->list[i].field(3);if(!meta)return error.fail(Status::INVALID,"file_sql: missing Parquet column metadata");
      const Node *path=meta->field(3);
      if(!path||path->list.size()!=1||path->list[0].text!=footer.columns[i].name||meta->number(1,-1)!=footer.columns[i].physical)
        return error.fail(Status::INVALID,"file_sql: mismatched Parquet column path/type");
      int64_t offset=meta->number(9,-1),dictionary=meta->number(11,-1),size=meta->number(7,-1);
      if(dictionary>=0)offset=std::min(offset,dictionary);
      if(offset<4||size<0||uint64_t(offset)>footer.footer_offset||uint64_t(size)>footer.footer_offset-uint64_t(offset)||meta->number(5,-1)!=rows)
        return error.fail(Status::INVALID,"file_sql: invalid Parquet column extent");
      Chunk chunk;chunk.offset=offset;chunk.end=offset+size;chunk.values=rows;chunk.codec=meta->number(4,-1);
      if(chunk.codec!=0&&chunk.codec!=1)return error.fail(Status::UNSUPPORTED,"file_sql: Parquet supports uncompressed and Snappy pages");
      group.chunks.push_back(chunk);
    }
    footer.rows+=group.rows;footer.groups.push_back(std::move(group));
  }
  if(footer.rows!=uint64_t(root.number(3)))return error.fail(Status::INVALID,"file_sql: inconsistent Parquet row counts");
  return Status::OK;
}

bool varint(const std::string &data,size_t &position,uint64_t &value)
{
  value=0;for(int i=0;i<10;++i){if(position>=data.size())return false;uint8_t b=data[position++];if(i==9&&b>1)return false;value|=uint64_t(b&127)<<(7*i);if(!(b&128))return true;}return false;
}
bool snappy(const std::string &compressed,size_t expected,std::string &out)
{
  size_t p=0;uint64_t declared;
  if(!varint(compressed,p,declared)||declared!=expected||expected>MAX_PAGE)return false;
  out.clear();out.reserve(expected);
  while(p<compressed.size()&&out.size()<expected){
    uint8_t tag=compressed[p++];size_t length=0,offset=0;
    if((tag&3)==0){
      length=tag>>2;
      if(length<60)++length;
      else {size_t bytes=length-59;if(p+bytes>compressed.size()||bytes>4)return false;length=0;for(size_t i=0;i<bytes;++i)length|=size_t(static_cast<unsigned char>(compressed[p++]))<<(8*i);++length;}
      if(length>expected-out.size()||length>compressed.size()-p)return false;
      out.append(compressed,p,length);p+=length;
    }else{
      if((tag&3)==1){if(p==compressed.size())return false;length=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|static_cast<unsigned char>(compressed[p++]);}
      else {size_t bytes=(tag&3)==2?2:4;length=1+(tag>>2);if(p+bytes>compressed.size())return false;for(size_t i=0;i<bytes;++i)offset|=size_t(static_cast<unsigned char>(compressed[p++]))<<(8*i);}
      if(offset==0||offset>out.size()||length>expected-out.size())return false;
      for(size_t i=0;i<length;++i)out.push_back(out[out.size()-offset]);
    }
  }
  return out.size()==expected&&p==compressed.size();
}
bool hybrid(const std::string &data,size_t &position,unsigned width,size_t count,std::vector<uint32_t> &out)
{
  if(width>32||count>MAX_VALUES)return false;out.clear();out.reserve(count);
  while(out.size()<count){
    uint64_t header;if(!varint(data,position,header)||header==0)return false;
    uint64_t run=header>>1;
    if(!(header&1)){
      size_t bytes=(width+7)/8;if(position+bytes>data.size()||run>MAX_VALUES*8)return false;
      uint32_t value=0;for(size_t i=0;i<bytes;++i)value|=uint32_t(static_cast<unsigned char>(data[position++]))<<(8*i);
      if(width<32&&value>=(uint32_t{1}<<width))return false;
      size_t amount=std::min<uint64_t>(run,count-out.size());out.insert(out.end(),amount,value);
    }else{
      if(run>MAX_VALUES||run*width>data.size()-position)return false;
      size_t values=run*8,bytes=run*width;
      for(size_t i=0;i<values&&out.size()<count;++i){uint32_t value=0;for(unsigned bit=0;bit<width;++bit){size_t offset=i*width+bit;value|=((static_cast<unsigned char>(data[position+offset/8])>>(offset%8))&1u)<<bit;}out.push_back(value);}
      position+=bytes;
    }
  }
  return true;
}
bool date_text(int64_t days,std::string &text)
{
  int64_t z=days+719468,era=(z>=0?z:z-146096)/146097;unsigned doe=static_cast<unsigned>(z-era*146097);
  unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t year=yoe+era*400;
  unsigned doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153,day=doy-(153*mp+2)/5+1,month=mp+(mp<10?3:-9);
  year+=month<=2;if(year<1||year>9999)return false;
  char buffer[32];std::snprintf(buffer,sizeof(buffer),"%04lld-%02u-%02u",static_cast<long long>(year),month,day);text=buffer;return true;
}
bool datetime_text(int64_t microseconds,std::string &text)
{
  const int64_t day_us=86400LL*1000000;int64_t days=microseconds/day_us,rem=microseconds%day_us;
  if(rem<0){--days;rem+=day_us;}if(!date_text(days,text))return false;
  char buffer[48];std::snprintf(buffer,sizeof(buffer)," %02lld:%02lld:%02lld.%06lld",
      static_cast<long long>(rem/3600000000LL),static_cast<long long>((rem/60000000)%60),
      static_cast<long long>((rem/1000000)%60),static_cast<long long>(rem%1000000));text+=buffer;return true;
}
bool plain(const std::string &data,size_t &position,size_t count,const PhysicalColumn &column,std::vector<Cell> &out)
{
  if(count>MAX_VALUES)return false;out.clear();out.reserve(count);
  if(column.physical==0){
    if((count+7)/8>data.size()-position)return false;
    for(size_t i=0;i<count;++i)out.push_back({(static_cast<unsigned char>(data[position+i/8])>>(i%8))&1?"true":"false",false});
    position+=(count+7)/8;return true;
  }
  for(size_t i=0;i<count;++i){
    Cell cell;cell.is_null=false;int64_t integer=0;double number=0;
    if(column.physical==1){if(position+4>data.size())return false;uint32_t bits=le32(data.data()+position);integer=(column.converted==13)?int64_t(bits):int64_t(static_cast<int32_t>(bits));position+=4;}
    else if(column.physical==2){if(position+8>data.size())return false;uint64_t bits=le64(data.data()+position);integer=static_cast<int64_t>(bits);position+=8;if(column.converted==14){cell.text=std::to_string(bits);out.push_back(std::move(cell));continue;}}
    else if(column.physical==3){
      if(position+12>data.size())return false;uint64_t nanos=le64(data.data()+position);uint32_t julian=le32(data.data()+position+8);position+=12;
      if(nanos>=86400000000000ULL||julian<1721426||julian>5373484)return false;
      integer=(int64_t(julian)-2440588)*86400000000LL+nanos/1000;
    }else if(column.physical==4){if(position+4>data.size())return false;uint32_t bits=le32(data.data()+position);float value;std::memcpy(&value,&bits,4);number=value;position+=4;}
    else if(column.physical==5){if(position+8>data.size())return false;uint64_t bits=le64(data.data()+position);std::memcpy(&number,&bits,8);position+=8;}
    else{
      size_t length=column.fixed_length;
      if(column.physical==6){if(position+4>data.size())return false;length=le32(data.data()+position);position+=4;}
      if(length>MAX_RECORD_SIZE||length>data.size()-position)return false;
      cell.text.assign(data,position,length);position+=length;if(!valid_utf8(cell.text))return false;out.push_back(std::move(cell));continue;
    }
    if(column.type==Type::DATE){if(integer < -719162||integer>2932896||!date_text(integer,cell.text))return false;}
    else if(column.type==Type::DATETIME){
      if(column.unit==1){if(integer<INT64_MIN/1000||integer>INT64_MAX/1000)return false;integer*=1000;}
      else if(column.unit==3)integer=integer/1000-(integer<0&&integer%1000!=0);
      if(!datetime_text(integer,cell.text))return false;
    }else if(column.type==Type::DOUBLE){if(!std::isfinite(number))return false;char buffer[64];std::snprintf(buffer,sizeof(buffer),"%.17g",number);cell.text=buffer;}
    else cell.text=std::to_string(integer);
    out.push_back(std::move(cell));
  }
  return true;
}

class ColumnCursor {
public:
  ColumnCursor(int descriptor,const PhysicalColumn &column,const Chunk &chunk):fd_(descriptor),column_(column),chunk_(chunk),offset_(chunk.offset){}
  size_t memory()const{size_t total=page_.size()*sizeof(Cell)+dictionary_.size()*sizeof(Cell);for(const Cell &v:page_)total+=v.text.capacity();for(const Cell &v:dictionary_)total+=v.text.capacity();return total;}
  Status next(Cell &cell,Error &error){
    while(position_==page_.size()){
      if(remaining_==0&&loaded_)return Status::END;
      Status status=load_page(error);if(status!=Status::OK)return status;
    }
    cell=page_[position_++];--remaining_;return Status::OK;
  }
private:
  Status bad(Error &error,const char *text){return error.fail(Status::INVALID,std::string("file_sql: invalid Parquet page: ")+text);}
  Status load_page(Error &error){
    if(!loaded_){remaining_=chunk_.values;loaded_=true;}
    page_.clear();position_=0;
    while(offset_<chunk_.end){
      std::string header_bytes;
      if(!read_at(fd_,offset_,std::min<uint64_t>(65536,chunk_.end-offset_),header_bytes))return bad(error,"truncated header");
      Node header;Compact parser(header_bytes);if(!parser.root(header))return bad(error,"corrupt header");
      int64_t compressed=header.number(3,-1),uncompressed=header.number(2,-1),kind=header.number(1,-1);
      if(compressed<0||uncompressed<0||compressed>MAX_PAGE||uncompressed>MAX_PAGE||parser.position()>chunk_.end-offset_||uint64_t(compressed)>chunk_.end-offset_-parser.position())return bad(error,"invalid size");
      offset_+=parser.position();std::string stored,payload;
      if(!read_at(fd_,offset_,compressed,stored))return bad(error,"truncated body");offset_+=compressed;
      if(kind==1)continue;
      const Node *page_header=header.field(kind==2?7:kind==3?8:5);
      if((kind!=0&&kind!=2&&kind!=3)||!page_header)return bad(error,"unsupported page type");
      int64_t values=page_header->number(1,-1);if(values<0||values>MAX_VALUES||(kind!=2&&values>remaining_))return bad(error,"invalid value count");
      size_t levels=0;
      if(kind==3){int64_t defs=page_header->number(5,-1),reps=page_header->number(6,-1);if(defs<0||reps!=0||defs>compressed||defs>uncompressed)return bad(error,"invalid V2 levels");levels=defs;}
      bool is_compressed=kind!=3||page_header->number(7,1)!=0;
      if(chunk_.codec==1&&is_compressed){std::string decoded;if(!snappy(stored.substr(levels),uncompressed-levels,decoded))return bad(error,"Snappy decompression failed");payload=stored.substr(0,levels)+decoded;}
      else {if(compressed!=uncompressed)return bad(error,"uncompressed size mismatch");payload=std::move(stored);}
      size_t position=0;
      if(kind==2){
        if(page_header->number(2,-1)!=0||!plain(payload,position,values,column_,dictionary_)||position!=payload.size())return bad(error,"invalid dictionary");
        continue;
      }
      std::vector<uint32_t> definitions;
      if(column_.optional){
        size_t length=levels;
        if(kind==0){if(payload.size()<4||page_header->number(3,-1)!=3)return bad(error,"invalid definition levels");length=le32(payload.data());position=4;}
        if(length>payload.size()-position)return bad(error,"truncated definition levels");
        std::string encoded=payload.substr(position,length);size_t cursor=0;
        if(!hybrid(encoded,cursor,1,values,definitions))return bad(error,"invalid definition run");position+=length;
      }else definitions.assign(values,1);
      size_t present=std::count(definitions.begin(),definitions.end(),1u);
      std::vector<Cell> decoded;int64_t encoding=page_header->number(2,-1);
      if(kind==3)encoding=page_header->number(4,-1);
      if(encoding==0){if(!plain(payload,position,present,column_,decoded))return bad(error,"invalid PLAIN encoding");}
      else if(encoding==2||encoding==8){
        if(position>=payload.size()&&present)return bad(error,"missing dictionary indices");
        unsigned width=position<payload.size()?static_cast<unsigned char>(payload[position++]):0;std::vector<uint32_t> indices;
        if(present&&!hybrid(payload,position,width,present,indices))return bad(error,"invalid dictionary indices");
        for(uint32_t index:indices){if(index>=dictionary_.size())return bad(error,"dictionary index out of range");decoded.push_back(dictionary_[index]);}
      }else if(encoding==3&&column_.physical==0){
        if(position+4>payload.size())return bad(error,"missing boolean RLE length");
        size_t length=le32(payload.data()+position);position+=4;
        if(length!=payload.size()-position)return bad(error,"invalid boolean RLE length");
        std::string encoded=payload.substr(position,length);size_t cursor=0;std::vector<uint32_t> bits;
        if(!hybrid(encoded,cursor,1,present,bits))return bad(error,"invalid boolean RLE values");
        for(uint32_t bit:bits){if(bit>1)return bad(error,"invalid boolean value");decoded.push_back({bit?"true":"false",false});}
        position+=length;
      }else return error.fail(Status::UNSUPPORTED,"file_sql: unsupported Parquet value encoding");
      if(position!=payload.size())return bad(error,"trailing encoded values");
      size_t cursor=0;page_.reserve(values);
      for(uint32_t level:definitions){if(level>1)return bad(error,"invalid null level");page_.push_back(level?std::move(decoded[cursor++]):Cell{});}
      if(kind==3&&page_header->number(2,-1)!=int64_t(values-present))return bad(error,"inconsistent null count");
      return Status::OK;
    }
    return remaining_==0?Status::END:bad(error,"column ended before row group");
  }
  int fd_;PhysicalColumn column_;Chunk chunk_;uint64_t offset_;bool loaded_=false;
  int64_t remaining_=0;std::vector<Cell> dictionary_,page_;size_t position_=0;
};
} // namespace parquet_detail

Status infer_parquet_schema(int fd,Definition &definition,Error &error)
{
  parquet_detail::Footer footer;Status status=parquet_detail::parse_footer(fd,footer,error);if(status!=Status::OK)return status;
  definition.columns.clear();definition.rows=footer.rows;
  for(const auto &column:footer.columns)definition.columns.push_back({column.name,column.type,column.optional,column.type==Type::VARCHAR?uint32_t(MAX_RECORD_SIZE):0u, static_cast<uint8_t>(column.type==Type::DATETIME?(column.unit==1?3:6):0)});
  return Status::OK;
}
struct ParquetReader::Impl {
  int fd=-1;parquet_detail::Footer footer;std::vector<size_t> projection;
  std::vector<std::unique_ptr<parquet_detail::ColumnCursor>> cursors;
  size_t group=0;uint64_t row=0;
};
ParquetReader::ParquetReader():impl_(new Impl){}
ParquetReader::~ParquetReader()=default;
Status ParquetReader::open(int fd,const Definition &definition,const std::vector<size_t> &projection,Error &error)
{
  impl_.reset(new Impl);impl_->fd=fd;
  Status status=parquet_detail::parse_footer(fd,impl_->footer,error);if(status!=Status::OK)return status;
  if(impl_->footer.rows!=definition.rows||impl_->footer.columns.size()!=definition.columns.size())return error.fail(Status::CHANGED,"file_sql: Parquet schema changed");
  for(size_t i=0;i<definition.columns.size();++i)if(impl_->footer.columns[i].name!=definition.columns[i].name||impl_->footer.columns[i].type!=definition.columns[i].type)return error.fail(Status::CHANGED,"file_sql: Parquet schema changed");
  impl_->projection=projection;
  if(projection.empty())for(size_t i=0;i<definition.columns.size();++i)impl_->projection.push_back(i);
  for(size_t index:impl_->projection)if(index>=definition.columns.size())return error.fail(Status::INVALID,"file_sql: invalid Parquet column projection");
  return Status::OK;
}
Status ParquetReader::next(std::vector<Cell> &row,Error &error)
{
  while(impl_->group<impl_->footer.groups.size()&&impl_->row==impl_->footer.groups[impl_->group].rows){++impl_->group;impl_->row=0;impl_->cursors.clear();}
  if(impl_->group==impl_->footer.groups.size())return Status::END;
  const auto &group=impl_->footer.groups[impl_->group];
  if(impl_->cursors.empty())for(size_t column:impl_->projection)impl_->cursors.emplace_back(new parquet_detail::ColumnCursor(impl_->fd,impl_->footer.columns[column],group.chunks[column]));
  row.assign(impl_->footer.columns.size(),Cell{});size_t memory=0;
  for(size_t i=0;i<impl_->projection.size();++i){
    Status status=impl_->cursors[i]->next(row[impl_->projection[i]],error);
    if(status==Status::END)return error.fail(Status::INVALID,"file_sql: Parquet column ended early");
    if(status!=Status::OK)return status;
    memory+=impl_->cursors[i]->memory();if(memory>parquet_detail::MAX_LIVE_PAGES)return error.fail(Status::LIMIT,"file_sql: Parquet live-page memory limit");
  }
  ++impl_->row;return Status::OK;
}
}}}
