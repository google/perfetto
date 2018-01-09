/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <inttypes.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"

#include "protos/trace_packet.pbzero.h"

namespace perfetto {

namespace {

class ProbesProducer : Producer {
 public:
  explicit ProbesProducer(base::TaskRunner*);
  ~ProbesProducer() override;

  // Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void CreateDataSourceInstance(DataSourceInstanceID,
                                const DataSourceConfig&) override;
  void TearDownDataSourceInstance(DataSourceInstanceID) override;

 private:
  base::TaskRunner* task_runner_;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint_;
};

ProbesProducer::ProbesProducer(base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      producer_endpoint_(ProducerIPCClient::Connect(PERFETTO_PRODUCER_SOCK_NAME,
                                                    this,
                                                    task_runner)) {
  base::ignore_result(task_runner_);
}

ProbesProducer::~ProbesProducer() = default;

void ProbesProducer::OnConnect() {
  PERFETTO_ILOG("Connected to tracing service. Registering data source");
  DataSourceDescriptor desc;
  desc.set_name("perfetto.test");
  producer_endpoint_->RegisterDataSource(desc, [](DataSourceID dsid) {
    PERFETTO_ILOG("Registered data source with ID: %" PRIu64, dsid);
  });
}

void ProbesProducer::OnDisconnect() {
  PERFETTO_ILOG("Disconnected from tracing service");
}

void ProbesProducer::CreateDataSourceInstance(DataSourceInstanceID dsid,
                                              const DataSourceConfig& cfg) {
  PERFETTO_ILOG("CreateDataSourceInstance(). Firstly sending some packets.");
  auto twriter = producer_endpoint_->CreateTraceWriter(1 /* target buffer */);
  for (int i = 0; i < 3; i++) {
    auto pack = twriter->NewTracePacket();
    char str[100];
    sprintf(str, "foooooooooooooooooooooo %d", i);
    pack->set_test(str, strlen(str));
  }

  PERFETTO_ILOG("Now trying to access ftrace.");

  base::ScopedFile tracing_on(open("/d/tracing/tracing_on", O_RDWR));
  base::ScopedFile enable(
      open("/d/tracing/events/sched/sched_switch/enable", O_RDWR));
  base::ScopedFile pipe(
      open("/d/tracing/per_cpu/cpu0/trace_pipe_raw", O_RDONLY));

  PERFETTO_ILOG("tracing_on: %d, wr: %zd", *tracing_on,
                write(*tracing_on, "1", 1));
  tracing_on.reset();

  PERFETTO_ILOG("oom/enable: %d, wr: %zd", *enable, write(*enable, "1", 1));
  enable.reset();

  char buf[4096] = {};
  fcntl(*pipe, F_SETFL, O_NONBLOCK);
  sleep(1);
  PERFETTO_ILOG("trace_pipe_raw: %d, rd: %zd", *pipe,
                read(*pipe, buf, sizeof(buf)));
  pipe.reset();
}

void ProbesProducer::TearDownDataSourceInstance(DataSourceInstanceID dsid) {
  PERFETTO_ILOG("TearDownDataSourceInstance()");
  base::ScopedFile tracing_on(open("/d/tracing/tracing_on", O_RDWR));
  write(*tracing_on, "0", 1);
}

}  // namespace

int __attribute__((visibility("default"))) ProbesMain(int argc, char** argv) {
  perfetto::base::UnixTaskRunner task_runner;
  perfetto::ProbesProducer producer(&task_runner);
  task_runner.Run();
  return 0;
}

}  // namespace perfetto
