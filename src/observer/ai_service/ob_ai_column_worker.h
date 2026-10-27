/** Copyright (c) 2026 OceanBase. */
#pragma once
#include "share/ob_thread_pool.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase { namespace observer {
class AiColumnWorker : public share::ObThreadPool
{
public:
  AiColumnWorker() : share::ObThreadPool(1), proxy_(nullptr) {}
  ~AiColumnWorker() { stop(); wait(); }
  int start_worker(common::ObMySQLProxy &proxy);
  void run1() override;
private:
  int process_one_group();
  common::ObMySQLProxy *proxy_;
};
}}
