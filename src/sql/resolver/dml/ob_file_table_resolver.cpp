/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/engine/basic/ob_file_scan_kernel.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string file_node_text(const ParseNode *node)
{
  if(node==nullptr)return "";
  if(node->type_==T_COLUMN_REF&&node->num_child_==3)return file_node_text(node->children_[2]);
  if(node->str_value_!=nullptr)return std::string(node->str_value_,node->str_len_);
  return "";
}
std::string file_lower(std::string value)
{
  for(char &c:value)if(c>='A'&&c<='Z')c+='a'-'A';return value;
}
bool file_literal(const ParseNode *node, const ParamStore *parameters,
                  std::string &value, bool string_only = false)
{
  if (node == nullptr) return false;
  if (node->type_ == T_QUESTIONMARK) {
    const int64_t index = node->value_;
    if (parameters == nullptr || index < 0 || index >= parameters->count()) return false;
    const ObObjParam &parameter = parameters->at(index);
    if (parameter.is_null()) return false;
    if (ob_is_string_type(parameter.get_type())) {
      const ObString text = parameter.get_string();
      value.assign(text.ptr(), text.length());
      return true;
    } else if (!string_only && ob_is_integer_type(parameter.get_type())) {
      value = parameter.is_unsigned_integer() ? std::to_string(parameter.get_uint64())
                                             : std::to_string(parameter.get_int());
      return true;
    }
    return false;
  }
  if (node->type_ == T_VARCHAR || node->type_ == T_CHAR) {
    value = file_node_text(node);
    return true;
  }
  if (string_only) return false;
  if (node->type_ == T_INT) { value = std::to_string(node->value_); return true; }
  if (node->type_ == T_BOOL) { value = node->value_ ? "true" : "false"; return true; }
  if (node->type_ == T_OP_NEG && node->num_child_ == 1) {
    if (!file_literal(node->children_[0], parameters, value)) return false;
    value = "-" + value;
    return true;
  }
  return false;
}

int parse_file_arguments(const ParseNode &tree, const ParamStore *parameters, filesql::Definition &definition)
{
  int ret=OB_SUCCESS;
  const std::string function=file_lower(file_node_text(tree.children_[0]));
  if(function=="read_csv")definition.format=filesql::Format::CSV;
  else if(function=="read_json"||function=="read_ndjson"||function=="read_jsonl")definition.format=filesql::Format::JSONL;
  else if(function=="read_parquet")definition.format=filesql::Format::PARQUET;
  else if(function=="glob")definition.format=filesql::Format::GLOB;
  else return OB_ERR_PARSE_SQL;
  const ParseNode *args=tree.children_[1];
  if (args == nullptr || args->num_child_ < 1 ||
      !file_literal(args->children_[0], parameters, definition.path, true)) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file SQL path must be a non-NULL string literal or bound string");
    return OB_INVALID_ARGUMENT;
  }
  std::set<std::string> keys;
  for(int64_t i=1;OB_SUCC(ret)&&i<args->num_child_;++i){
    const ParseNode *arg=args->children_[i];std::string value;
    if(arg==nullptr||arg->type_!=T_OP_EQ||arg->num_child_!=2||!file_literal(arg->children_[1],parameters,value))return OB_ERR_PARSE_SQL;
    std::string key=file_lower(file_node_text(arg->children_[0]));
    if(!keys.insert(key).second)return OB_INVALID_ARGUMENT;
    if(key=="sample_size"){
      if(value!="-1"){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL requires full schema inference (sample_size=-1)");}
    }else if(key=="format"){
      if(definition.format!=filesql::Format::JSONL||(file_lower(value)!="newline_delimited"&&file_lower(value)!="ndjson"))return OB_ERR_PARSE_SQL;
    }else if(key=="header"&&definition.format==filesql::Format::CSV){
      if(value=="true"||value=="1")definition.options.header=true;
      else if(value=="false"||value=="0")definition.options.header=false;
      else return OB_INVALID_ARGUMENT;
    }else if((key=="delim"||key=="delimiter"||key=="sep")&&definition.format==filesql::Format::CSV){
      if(value.size()!=1||value[0]=='\n'||value[0]=='\r'||value[0]=='\0')return OB_INVALID_ARGUMENT;
      definition.options.delimiter=value[0];
    }else if((key=="quote"||key=="escape")&&definition.format==filesql::Format::CSV){
      if(value.size()>1||(!value.empty()&&(value[0]=='\n'||value[0]=='\r'||value[0]=='\0')))return OB_INVALID_ARGUMENT;
      if(key=="quote")definition.options.quote=value.empty()?'\0':value[0];
      else definition.options.escape=value.empty()?'\0':value[0];
    }else if((key=="nullstr"||key=="null_string")&&definition.format==filesql::Format::CSV){
      definition.options.null_text=value;
    }else return OB_ERR_PARSE_SQL;
  }
  if(definition.options.quote!=0&&definition.options.delimiter==definition.options.quote)return OB_INVALID_ARGUMENT;
  return ret;
}
}

int ObDMLResolver::resolve_file_table_item(const ParseNode &tree,TableItem *&table_item)
{
  int ret=OB_SUCCESS;
  try{
    filesql::Definition definition;filesql::Error error;
    ObDMLStmt *stmt=get_stmt();ObConstRawExpr *metadata=nullptr;TableItem *item=nullptr;
    if(tree.num_child_!=3||session_info_==nullptr||stmt==nullptr)return OB_ERR_UNEXPECTED;
    if(OB_FAIL(filesql::check_enabled(*session_info_))){
    }else if(OB_FAIL(parse_file_arguments(tree,params_.param_list_,definition))){
    }else if(OB_FAIL(filesql::runtime_secure_root(*allocator_,definition.secure_root))){
    }else if(OB_FAIL(filesql::sql_error(filesql::infer_schema(definition,error),error))){
    }else{
      std::string encoded;ObString stored;
      if(OB_FAIL(filesql::sql_error(filesql::encode_definition(definition,encoded,error),error))){
      }else if(OB_FAIL(ob_write_string(*allocator_,ObString(encoded.size(),encoded.data()),stored))){
      }else if(OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_,ObVarcharType,stored,CS_TYPE_BINARY,metadata))){
      }else if(OB_FAIL(metadata->formalize(session_info_))){
      }else if(OB_ISNULL(item=stmt->create_table_item(*allocator_))){ret=OB_ALLOCATE_MEMORY_FAILED;
      }else{
        ObString alias;
        if(tree.children_[2]!=nullptr){
          std::string name=file_node_text(tree.children_[2]);
          ret=ob_write_string(*allocator_,ObString(name.size(),name.data()),alias);
        }else ret=stmt->generate_func_table_name(*allocator_,alias);
        if(OB_SUCC(ret)){
          item->table_name_=alias;item->alias_name_=alias;item->table_id_=generate_table_id();
          item->type_=TableItem::FUNCTION_TABLE;item->function_table_expr_=metadata;item->is_file_table_=true;
          if(OB_FAIL(stmt->add_table_item(session_info_,item))){
          }else{
            ObSEArray<ColumnItem,16> columns;
            ret=resolve_file_table_columns(*item,columns);
          }
        }
        if(OB_SUCC(ret)){
          ObQueryHint &hint=stmt->get_query_ctx()->get_query_hint_for_update();
          const_cast<ObGlobalHint&>(hint.get_global_hint()).merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
          table_item=item;
        }
      }
    }
  }catch(const std::bad_alloc &){ret=OB_ALLOCATE_MEMORY_FAILED;}
  return ret;
}

int ObDMLResolver::resolve_file_table_columns(const TableItem &table_item,ObIArray<ColumnItem> &columns)
{
  int ret=OB_SUCCESS;
  const ObConstRawExpr *constant=static_cast<const ObConstRawExpr *>(table_item.function_table_expr_);
  ObString encoded=constant->get_value().get_string();
  filesql::Definition definition;filesql::Error error;
  if(OB_FAIL(filesql::sql_error(filesql::decode_definition(std::string(encoded.ptr(),encoded.length()),definition,error),error))){
  }else{
    for(size_t i=0;OB_SUCC(ret)&&i<definition.columns.size();++i){
      const filesql::Column &column=definition.columns[i];ObObjMeta meta;ObAccuracy accuracy;
      switch(column.type){
        case filesql::Type::NULL_VALUE:meta.set_null();break;
        case filesql::Type::BIGINT:meta.set_int();break;
        case filesql::Type::DOUBLE:meta.set_double();break;
        case filesql::Type::BOOLEAN:meta.set_tinyint();break;
        case filesql::Type::VARCHAR:meta.set_varchar();meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);meta.set_collation_level(CS_LEVEL_IMPLICIT);break;
        case filesql::Type::DATE:meta.set_date();break;
        case filesql::Type::DATETIME:meta.set_datetime();break;
      }
      accuracy=ObAccuracy::DDL_DEFAULT_ACCURACY[meta.get_type()];
      if(column.type==filesql::Type::VARCHAR)accuracy.set_length(std::max<uint32_t>(1,column.max_length));
      if(column.type==filesql::Type::DATETIME)accuracy.set_scale(column.fractional_scale);
      ObString name;
      if(OB_FAIL(ob_write_string(*allocator_,ObString(column.name.size(),column.name.data()),name))){
      }else{
        ColumnItem *item=get_stmt()->get_column_item(table_item.table_id_,name);
        if(item==nullptr)ret=resolve_function_table_column_item(table_item,meta,accuracy,name,OB_APP_MIN_COLUMN_ID+i,item);
        if(OB_SUCC(ret)){
          if(!column.nullable){ObRawExprResType type=item->expr_->get_result_type();type.set_result_flag(NOT_NULL_FLAG);item->expr_->set_result_type(type);}
          ret=columns.push_back(*item);
        }
      }
    }
  }
  return ret;
}
}}
