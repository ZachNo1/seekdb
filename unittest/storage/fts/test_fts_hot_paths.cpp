#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "storage/fts/dict/ob_ft_dat_dict.h"
#include "share/text_analysis/ob_token_stream.h"
#include "data_plane/ob_order_perserving_encoder.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;

TEST(FtsHotPaths, binary_id_order_matches_generic_comparator)
{
  ObArenaAllocator allocator("FtsCompareTest");
  ObDatum left,right;
  ObFixedArray<const ObDatum *, ObIAllocator> ids;
  ids.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS,ids.init(2));
  ASSERT_EQ(OB_SUCCESS,ids.push_back(&left));
  ASSERT_EQ(OB_SUCCESS,ids.push_back(&right));
  ObSRMergeItem l,r;l.iter_idx_=0;r.iter_idx_=1;
  std::mt19937 random(20261027);
  for (ObCollationType collation : {CS_TYPE_BINARY,CS_TYPE_UTF8MB4_GENERAL_CI}) {
    oceanbase::sql::ObDatumMeta meta(ObVarcharType,collation,0);
    ObSRMergeCmp optimized;
    ASSERT_EQ(OB_SUCCESS,optimized.init(meta,&ids));
    auto reference=ObDatumFuncs::get_basic_func(meta.type_,meta.cs_type_)->null_first_cmp_;
    for(int i=0;i<10000;++i) {
      std::string a(random()%33,'\0'),b(random()%33,'\0');
      for(char &ch:a)ch=collation==CS_TYPE_BINARY ? random()%256 : ' '+random()%95;
      for(char &ch:b)ch=collation==CS_TYPE_BINARY ? random()%256 : ' '+random()%95;
      if(i%5==0)b=a;
      if(i%7==0)b=a+" ";
      left.set_string(a.data(),a.size());right.set_string(b.data(),b.size());
      if(i%11==0)left.set_null();
      if(i%13==0)right.set_null();
      int expected=0;int64_t actual=0;
      ASSERT_EQ(OB_SUCCESS,reference(left,right,expected,nullptr));
      ASSERT_EQ(OB_SUCCESS,optimized.cmp(l,r,actual));
      ASSERT_EQ((expected>0)-(expected<0),(actual>0)-(actual<0));
    }
  }
  oceanbase::sql::ObDatumMeta meta(ObUInt64Type,CS_TYPE_BINARY,0);
  ObSRMergeCmp optimized;
  ASSERT_EQ(OB_SUCCESS,optimized.init(meta,&ids));
  left.set_uint(UINT64_MAX);right.set_uint(0);
  int64_t result=0;
  ASSERT_EQ(OB_SUCCESS,optimized.cmp(l,r,result));
  EXPECT_GT(result,0);
}

TEST(FtsHotPaths, dictionary_collision_and_wraparound)
{
  ObArenaAllocator allocator("FtsDictTest");
  constexpr uint32_t count=1000;
  auto *map=static_cast<ObArrayHashMap *>(allocator.alloc(ObArrayHashMap::estimate_size(count)));
  ASSERT_NE(nullptr,map);
  ASSERT_EQ(OB_SUCCESS,map->init(count));
  for(uint32_t key=1;key<=count;++key) {
    ASSERT_EQ(OB_SUCCESS,map->insert(ObString(sizeof(key),reinterpret_cast<const char *>(&key)),key));
  }
  for(uint32_t key=1;key<=count+300;++key) {
    ObFTSingleWord word;
    ASSERT_EQ(OB_SUCCESS,word.set_word(reinterpret_cast<const char *>(&key),sizeof(key)));
    ObFTWordCode code=0;
    if(key<=count) {
      ASSERT_EQ(OB_SUCCESS,map->find(word,code));
      EXPECT_EQ(key,code);
    } else { EXPECT_EQ(OB_ENTRY_NOT_EXIST,map->find(word,code)); }
  }
}

namespace {
std::vector<std::string> reference_tokens(const std::string &text,const ObCharsetInfo *cs)
{
  std::vector<std::string> result;
  size_t position=0;
  const auto delimiter=[&]() {
    return ob_isspace(cs,text[position]) || ob_iscntrl(cs,text[position]) || ob_ispunct(cs,text[position]);
  };
  while(position<text.size()) {
    while(position<text.size() && delimiter()) {
      auto width=ob_mbcharlen_ptr(cs,text.data()+position,text.data()+text.size());
      if(width==0 || width>text.size()-position)return result;
      position+=width;
    }
    const size_t start=position;
    while(position<text.size() && !delimiter()) {
      auto width=ob_mbcharlen_ptr(cs,text.data()+position,text.data()+text.size());
      if(width==0 || width>text.size()-position) {
        if(position>start)result.emplace_back(text.substr(start,position-start));
        return result;
      }
      position+=width;
    }
    if(position>start)result.emplace_back(text.substr(start,position-start));
  }
  return result;
}
}

TEST(FtsHotPaths, ascii_scan_preserves_multibyte_and_invalid_suffix_semantics)
{
  ASSERT_EQ(OB_SUCCESS,ObCharset::init_charset());
  const std::vector<std::string> inputs={"Simple ASCII: words, punctuation!\t123\nEND", "中文 English 😀 café",
    std::string("abc\xf0\x9f",5),std::string("\0A B\0C",6),std::string("\xd6\xd0 hello",8),
    std::string("\0H\0i\0 \0T",8),std::string("A\0 \0B\0",6)};
  for(ObCollationType collation : {CS_TYPE_UTF8MB4_BIN,CS_TYPE_BINARY}) {
    const ObCharsetInfo *cs=ObCharset::get_charset(collation);
    ASSERT_NE(nullptr,cs);
    for(const auto &text:inputs) {
      ObTextWhitespaceTokenizer tokenizer;
      ObDatum document;document.set_string(text.data(),text.size());
      ASSERT_EQ(OB_SUCCESS,tokenizer.open(document,cs));
      std::vector<std::string> actual;
      int ret=OB_SUCCESS;ObDatum token;int64_t frequency=0;
      while((ret=tokenizer.get_next(token,frequency))==OB_SUCCESS) {
        EXPECT_EQ(1,frequency);
        actual.emplace_back(token.ptr_,static_cast<size_t>(token.len_));
      }
      ASSERT_EQ(OB_ITER_END,ret);
      EXPECT_EQ(reference_tokens(text,cs),actual);
    }
  }
}


TEST(FtsHotPaths, varchar_encoded_sort_matches_collation)
{
  const std::vector<std::string> pieces={"", "a", "A", "Z", "z", " ", "  ", "é", "e", "中文", "😀", std::string("\0",1)};
  std::mt19937 random(72133);
  for (ObCollationType collation : {CS_TYPE_BINARY,CS_TYPE_UTF8MB4_BIN,CS_TYPE_UTF8MB4_GENERAL_CI}) {
    auto compare=ObDatumFuncs::get_basic_func(ObVarcharType,collation)->null_first_cmp_;
    for(int i=0;i<10000;++i) {
      std::string a,b;
      for(unsigned j=0,n=random()%9;j<n;++j)a+=pieces[random()%pieces.size()];
      for(unsigned j=0,n=random()%9;j<n;++j)b+=pieces[random()%pieces.size()];
      if(i%4==0)b=a+" ";
      ObDatum left,right;left.set_string(a.data(),a.size());right.set_string(b.data(),b.size());
      if(i%11==0)left.set_null();if(i%13==0)right.set_null();
      ObEncParam lp,rp;lp.type_=rp.type_=ObVarcharType;lp.cs_type_=rp.cs_type_=collation;
      lp.is_asc_=rp.is_asc_=true;lp.is_nullable_=rp.is_nullable_=true;lp.is_null_first_=rp.is_null_first_=true;
      lp.is_memcmp_=rp.is_memcmp_=false;lp.is_simdopt_=rp.is_simdopt_=false;
      unsigned char lbuf[4096],rbuf[4096];int64_t ln=0,rn=0;int expected=0;
      ASSERT_EQ(OB_SUCCESS,ObSortkeyConditioner::process_key_conditioning(left,lbuf,sizeof(lbuf),ln,lp));
      ASSERT_EQ(OB_SUCCESS,ObSortkeyConditioner::process_key_conditioning(right,rbuf,sizeof(rbuf),rn,rp));
      ASSERT_TRUE(lp.is_valid_uni_);ASSERT_TRUE(rp.is_valid_uni_);
      ASSERT_EQ(OB_SUCCESS,compare(left,right,expected,nullptr));
      int actual=memcmp(lbuf,rbuf,std::min(ln,rn));if(actual==0)actual=(ln>rn)-(ln<rn);
      ASSERT_EQ((expected>0)-(expected<0),(actual>0)-(actual<0))<<"collation="<<collation<<" left="<<a<<" right="<<b;
    }
  }
}
