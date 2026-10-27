/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_kernel.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"

namespace oceanbase { namespace sql { namespace filesql {
using namespace common;
int check_enabled(ObSQLSessionInfo &session)
{
  int ret=OB_SUCCESS;bool enabled=false;
  if(OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled))){
  }else if(!enabled){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled; SET SESSION enable_file_sql=ON");}
  return ret;
}
int runtime_secure_root(common::ObIAllocator &allocator,std::string &root)
{
  int ret=OB_SUCCESS;common::ObString value;
  if(OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
      *GCTX.schema_service_,share::SYS_VAR_SECURE_FILE_PRIV,allocator,value))){
  }else{root.assign(value.ptr(),value.length());}
  return ret;
}
int sql_error(Status status,const Error &error)
{
  int ret=OB_SUCCESS;
  if(status==Status::END)ret=OB_ITER_END;
  else if(status!=Status::OK){ret=OB_INVALID_ARGUMENT;LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.message.c_str());}
  return ret;
}
}}}
