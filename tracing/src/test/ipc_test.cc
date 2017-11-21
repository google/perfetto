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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "base/logging.h"
#include "base/test/test_task_runner.h"
#include "tracing/core/data_source_config.h"
#include "tracing/core/data_source_descriptor.h"
#include "tracing/core/producer.h"
#include "tracing/core/service.h"
#include "tracing/ipc/producer_ipc_client.h"
#include "tracing/ipc/service_ipc_host.h"
#include "tracing/src/core/service_impl.h"
#include "tracing/src/ipc/posix_shared_memory.h"
#include "tracing/src/ipc/producer/producer_ipc_client_impl.h"
#include "tracing/src/ipc/service/service_ipc_host_impl.h"

namespace perfetto {

namespace {

const char kSocketName[] = "/tmp/perfetto-ipc-test.sock";

class TestProducer : public Producer {
 public:
  void OnConnect() override {
    PERFETTO_DLOG("Connected as Producer");
    if (on_connect)
      on_connect();
  }

  void OnDisconnect() override {
    PERFETTO_DLOG("Disconnected from tracing service");
  }

  void CreateDataSourceInstance(DataSourceInstanceID dsid,
                                const DataSourceConfig& cfg) override {
    PERFETTO_DLOG(
        "The tracing service requested us to start a new data source %" PRIu64
        ", config: %s",
        dsid, cfg.trace_category_filters.c_str());
  }

  void TearDownDataSourceInstance(DataSourceInstanceID instance_id) override {
    PERFETTO_DLOG(
        "The tracing service requested us to shutdown the data source %" PRIu64,
        instance_id);
  }

  std::function<void()> on_connect;
};

void __attribute__((noreturn)) ProducerMain() {
  base::TestTaskRunner task_runner;
  TestProducer producer;
  std::unique_ptr<Service::ProducerEndpoint> endpoint =
      ProducerIPCClient::Connect(kSocketName, &producer, &task_runner);
  producer.on_connect = task_runner.CreateCheckpoint("connect");
  task_runner.RunUntilCheckpoint("connect");

  for (int i = 0; i < 3; i++) {
    DataSourceDescriptor descriptor;
    descriptor.name = "perfetto.test.data_source";
    auto reg_checkpoint =
        task_runner.CreateCheckpoint("register" + std::to_string(i));
    auto on_register = [reg_checkpoint](DataSourceID id) {
      printf("Service acked RegisterDataSource() with ID %" PRIu64 "\n", id);
      reg_checkpoint();
    };
    endpoint->RegisterDataSource(descriptor, on_register);
    task_runner.RunUntilCheckpoint("register" + std::to_string(i));

    auto* ipc_client = static_cast<ProducerIPCClientImpl*>(endpoint.get());
    void* shm = ipc_client->shared_memory()->start();
    char buf[32];
    memcpy(buf, shm, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    printf("Shared Memory contents: \"%s\"\n", buf);
  }
  task_runner.Run();
}

void __attribute__((noreturn)) ServiceMain() {
  unlink(kSocketName);
  base::TestTaskRunner task_runner;
  std::unique_ptr<ServiceIPCHostImpl> host(static_cast<ServiceIPCHostImpl*>(
      ServiceIPCHost::CreateInstance(&task_runner).release()));

  class Observer : public Service::ObserverForTesting {
   public:
    explicit Observer(ServiceImpl* svc) : svc_(svc) {}
    void OnProducerConnected(ProducerID prid) override {
      printf("Producer connected: ID=%" PRIu64 "\n", prid);
    }

    void OnProducerDisconnected(ProducerID prid) override {
      printf("Producer disconnected: ID=%" PRIu64 "\n", prid);
    }

    void OnDataSourceRegistered(ProducerID prid, DataSourceID dsid) override {
      printf("Data source registered, Producer=%" PRIu64 " DataSource=%" PRIu64
             "\n",
             prid, dsid);
      DataSourceConfig cfg;
      cfg.trace_category_filters = "foo,bar";
      SharedMemory* shm = svc_->GetProducer(prid)->shared_memory();
      char shm_contents[32];
      sprintf(shm_contents, "shmem @ iteration %" PRIu64, dsid);
      memcpy(shm->start(), shm_contents, sizeof(shm_contents));
      svc_->GetProducer(prid)->producer()->CreateDataSourceInstance(42, cfg);
    }

    void OnDataSourceUnregistered(ProducerID prid, DataSourceID dsid) override {
      printf("Data source unregistered, Producer=%" PRIu64
             " DataSource=%" PRIu64 "\n",
             prid, dsid);
    }

    ServiceImpl* svc_;
  };

  host->Start(kSocketName);
  Observer observer(static_cast<ServiceImpl*>(host->service_for_testing()));
  host->service_for_testing()->set_observer_for_testing(&observer);
  task_runner.Run();
}

}  // namespace.
}  // namespace perfetto

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "producer"))
    perfetto::ProducerMain();
  if (argc == 2 && !strcmp(argv[1], "service"))
    perfetto::ServiceMain();

  fprintf(stderr, "Usage: %s producer | service\n", argv[0]);
  return 1;
}
