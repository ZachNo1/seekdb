/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/engine/basic/ob_file_scan_kernel.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string copy_node_string(const ParseNode *node)
{
  return node && node->str_value_ ? std::string(node->str_value_, node->str_len_) : "";
}
std::string copy_lower(std::string value)
{
  for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
bool copy_literal(const ParseNode *node,const ParamStore *params,std::string &value)
{
  if (!node) return false;
  if (node->type_ == T_VARCHAR || node->type_ == T_CHAR || node->type_ == T_IDENT) { value=copy_node_string(node); return true; }
  if (node->type_ == T_BOOL || node->type_ == T_INT) { value=std::to_string(node->value_); return true; }
  if (node->type_ == T_QUESTIONMARK && params && node->value_ >= 0 && node->value_ < params->count()) {
    const ObObjParam &param=params->at(node->value_);
    if (param.is_null()) return false;
    if (ob_is_string_type(param.get_type())) { const ObString text=param.get_string(); value.assign(text.ptr(),text.length()); return true; }
    if (ob_is_integer_type(param.get_type())) { value=std::to_string(param.get_int()); return true; }
  }
  return false;
}
}
int ObSelectResolver::resolve_file_copy(const ParseNode *node,ObSelectIntoItem &into_item)
{
  int ret=OB_SUCCESS;
  filesql::Definition definition; filesql::Error error;
  definition.options.header=false;
  if (OB_FAIL(filesql::check_enabled(*session_info_))) {
  } else if (node==nullptr || node->num_child_!=2 || !copy_literal(node->children_[0],params_.param_list_,definition.path)) {
    ret=OB_ERR_PARSE_SQL;
  } else {
    std::set<std::string> keys;
    const ParseNode *options=node->children_[1];
    for (int64_t i=0;OB_SUCC(ret)&&options&&i<options->num_child_;++i) {
      const ParseNode *option=options->children_[i];std::string value;
      if (!option || option->num_child_!=2 || !copy_literal(option->children_[1],params_.param_list_,value)) {ret=OB_ERR_PARSE_SQL;break;}
      const std::string key=copy_lower(copy_node_string(option->children_[0]));
      if (!keys.insert(key).second) {ret=OB_INVALID_ARGUMENT;break;}
      if (key=="format") {if(copy_lower(value)!="csv")ret=OB_NOT_SUPPORTED;}
      else if(key=="header") {
        value=copy_lower(value);
        if(value=="true"||value=="1")definition.options.header=true;
        else if(value=="false"||value=="0")definition.options.header=false;
        else ret=OB_INVALID_ARGUMENT;
      } else if(key=="delimiter"||key=="delim"||key=="sep") {
        if(value.size()!=1||value[0]=='\n'||value[0]=='\r'||value[0]==0)ret=OB_INVALID_ARGUMENT;
        else definition.options.delimiter=value[0];
      } else if(key=="quote"||key=="escape") {
        if(value.size()>1||(!value.empty()&&(value[0]=='\n'||value[0]=='\r'||value[0]==0)))ret=OB_INVALID_ARGUMENT;
        else if(key=="quote")definition.options.quote=value.empty()?0:value[0];
        else definition.options.escape=value.empty()?0:value[0];
      } else if(key=="nullstr"||key=="null_string")definition.options.null_text=value;
      else ret=OB_ERR_PARSE_SQL;
    }
    if(OB_SUCC(ret)) {
      std::string encoded;
      if(OB_FAIL(filesql::sql_error(filesql::encode_definition(definition,encoded,error),error))) {
      } else {
        encoded.insert(0,filesql::COPY_PREFIX);
        ObString path;
        if(OB_FAIL(ob_write_string(*allocator_,ObString(definition.path.size(),definition.path.data()),path))) {
        } else if(OB_FAIL(ob_write_string(*allocator_,ObString(encoded.size(),encoded.data()),into_item.external_properties_))) {
        } else {
          into_item.outfile_name_.set_varchar(path);
          into_item.is_single_=true;
          ObQueryHint &hint=get_stmt()->get_query_ctx()->get_query_hint_for_update();
          const_cast<ObGlobalHint&>(hint.get_global_hint()).merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
        }
      }
    }
  }
  return ret;
}
}}
