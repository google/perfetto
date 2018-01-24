/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <gtest/gtest.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <thread>

#include "perfetto/base/logging.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/tracing/ipc/service_ipc_host.h"

#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/ftrace_producer.h"
#include "test/fake_consumer.h"
#include "test/fake_producer.h"

#if BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include "perfetto/base/android_task_runner.h"
#endif

namespace perfetto {

#if BUILDFLAG(PERFETTO_ANDROID_BUILD)
using PlatformTaskRunner = base::AndroidTaskRunner;
#else
using PlatformTaskRunner = base::UnixTaskRunner;
#endif

// If we're building on Android but not as a CTS test, create the the producer
// and consumer socket in a world writable directory so permissions are not a
// problem.
#if BUILDFLAG(OS_ANDROID) && !BUILDFLAG(PERFETTO_ANDROID_BUILD)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#define TEST_CONSUMER_SOCK_NAME "/data/local/tmp/traced_consumer"
#else
#define TEST_PRODUCER_SOCK_NAME PERFETTO_PRODUCER_SOCK_NAME
#define TEST_CONSUMER_SOCK_NAME PERFETTO_CONSUMER_SOCK_NAME
#endif

class PerfettoTest : public ::testing::Test {
 public:
  PerfettoTest() {}
  ~PerfettoTest() override = default;

 protected:
  class ThreadDelegate {
   public:
    virtual ~ThreadDelegate() = default;

    // Invoke on the target thread before the message loop is started.
    virtual void Initialize(base::TaskRunner* task_runner) = 0;
  };

  class TaskRunnerThread {
   public:
    TaskRunnerThread() = default;
    ~TaskRunnerThread() {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (runner_)
          runner_->Quit();
      }

      if (thread_.joinable())
        thread_.join();
    }

    // Blocks until the thread has been created and Initialize() has been
    // called.
    void Start(std::unique_ptr<ThreadDelegate> delegate) {
      // Begin holding the lock for the condition variable.
      std::unique_lock<std::mutex> lock(mutex_);

      // Start the thread.
      PERFETTO_DCHECK(!runner_);
      thread_ = std::thread(&TaskRunnerThread::Run, this, std::move(delegate));

      // Wait for runner to be ready.
      ready_.wait_for(lock, std::chrono::seconds(10),
                      [this]() { return runner_ != nullptr; });
    }

   private:
    void Run(std::unique_ptr<ThreadDelegate> delegate) {
      // Create the task runner and execute the specicalised code.
      base::PlatformTaskRunner task_runner;
      delegate->Initialize(&task_runner);

      // Pass the runner back to the main thread.
      {
        std::unique_lock<std::mutex> lock(mutex_);
        runner_ = &task_runner;
      }

      // Notify the main thread that the runner is ready.
      ready_.notify_one();

      // Spin the loop.
      task_runner.Run();

      // Ensure we clear out the delegate before runner goes out
      // of scope.
      delegate.reset();

      // Cleanup the runner.
      {
        std::unique_lock<std::mutex> lock(mutex_);
        runner_ = nullptr;
      }
    }

    std::thread thread_;
    std::condition_variable ready_;

    // All variables below this point are protected by |mutex_|.
    std::mutex mutex_;
    base::PlatformTaskRunner* runner_ = nullptr;
  };

  // This is used only in standalone integrations tests. In CTS mode (i.e. when
  // PERFETTO_ANDROID_BUILD) this code is not used and instead the system
  // daemons are used
  class ServiceDelegate : public ThreadDelegate {
   public:
    ServiceDelegate() = default;
    ~ServiceDelegate() override = default;

    void Initialize(base::TaskRunner* task_runner) override {
      svc_ = ServiceIPCHost::CreateInstance(task_runner);
      unlink(TEST_PRODUCER_SOCK_NAME);
      unlink(TEST_CONSUMER_SOCK_NAME);
      svc_->Start(TEST_PRODUCER_SOCK_NAME, TEST_CONSUMER_SOCK_NAME);
    }

   private:
    std::unique_ptr<ServiceIPCHost> svc_;
  };

  // This is used only in standalone integrations tests. In CTS mode (i.e. when
  // PERFETTO_ANDROID_BUILD) this code is not used and instead the system
  // daemons are used.
  class FtraceProducerDelegate : public ThreadDelegate {
   public:
    FtraceProducerDelegate() = default;
    ~FtraceProducerDelegate() override = default;

    void Initialize(base::TaskRunner* task_runner) override {
      producer_.reset(new FtraceProducer);
      producer_->Connect(TEST_PRODUCER_SOCK_NAME, task_runner);
    }

   private:
    std::unique_ptr<FtraceProducer> producer_;
  };

  class FakeProducerDelegate : public ThreadDelegate {
   public:
    FakeProducerDelegate() = default;
    ~FakeProducerDelegate() override = default;

    void Initialize(base::TaskRunner* task_runner) override {
      producer_.reset(new FakeProducer("android.perfetto.FakeProducer"));
      producer_->Connect(TEST_PRODUCER_SOCK_NAME, task_runner);
    }

   private:
    std::unique_ptr<FakeProducer> producer_;
  };
};

// TODO(lalitm): reenable this when we have a solution for running ftrace
// on travis.
TEST_F(PerfettoTest, DISABLED_TestFtraceProducer) {
  base::TestTaskRunner task_runner;
  auto finish = task_runner.CreateCheckpoint("no.more.packets");

  // Setip the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(200);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("com.google.perfetto.ftrace");
  ds_config->set_target_buffer(0);

  // Setup the config for ftrace.
  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_event_names() = "sched_switch";
  *ftrace_config->add_event_names() = "bar";

  // Create the function to handle packets as they come in.
  uint64_t total = 0;
  auto function = [&total, &finish](std::vector<TracePacket> packets,
                                    bool has_more) {
    if (has_more) {
      for (auto& packet : packets) {
        packet.Decode();
        ASSERT_TRUE(packet->has_ftrace_events());
        for (int ev = 0; ev < packet->ftrace_events().event_size(); ev++) {
          ASSERT_TRUE(packet->ftrace_events().event(ev).has_sched_switch());
        }
      }
      total += packets.size();

      // TODO(lalitm): renable this when stiching inside the service is present.
      // ASSERT_FALSE(packets->empty());
    } else {
      ASSERT_GE(total, static_cast<uint64_t>(sysconf(_SC_NPROCESSORS_CONF)));
      ASSERT_TRUE(packets.empty());
      finish();
    }
  };

// If we're building with the Android platform (i.e. CTS), we expect that
// the service and ftrace producer both exist and are already running.
// TODO(lalitm): maybe add an additional build flag for CTS.
#if !BUILDFLAG(PERFETTO_ANDROID_BUILD)
  TaskRunnerThread service_thread;
  service_thread.Start(std::unique_ptr<ServiceDelegate>(new ServiceDelegate));

  TaskRunnerThread producer_thread;
  producer_thread.Start(
      std::unique_ptr<FtraceProducerDelegate>(new FtraceProducerDelegate));
#endif

  // Finally, make the consumer connect to the service.
  FakeConsumer consumer(trace_config, std::move(function), &task_runner);
  consumer.Connect(TEST_CONSUMER_SOCK_NAME);

  task_runner.RunUntilCheckpoint("no.more.packets");
}

TEST_F(PerfettoTest, TestFakeProducer) {
  base::TestTaskRunner task_runner;
  auto finish = task_runner.CreateCheckpoint("no.more.packets");

  // Setip the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(200);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  // Create the function to handle packets as they come in.
  uint64_t total = 0;
  auto function = [&total, &finish](std::vector<TracePacket> packets,
                                    bool has_more) {
    if (has_more) {
      for (auto& packet : packets) {
        packet.Decode();
        ASSERT_TRUE(packet->has_test());
        ASSERT_EQ(packet->test(), "test");
      }
      total += packets.size();

      // TODO(lalitm): renable this when stiching inside the service is present.
      // ASSERT_FALSE(packets->empty());
    } else {
      ASSERT_EQ(total, 10u);
      ASSERT_TRUE(packets.empty());
      finish();
    }
  };

// If we're building with the Android platform (i.e. CTS), we expect that
// the service and ftrace producer both exist and are already running.
// TODO(lalitm): maybe add an additional build flag for CTS.
#if !BUILDFLAG(PERFETTO_ANDROID_BUILD)
  TaskRunnerThread service_thread;
  service_thread.Start(std::unique_ptr<ServiceDelegate>(new ServiceDelegate));

  TaskRunnerThread producer_thread;
  producer_thread.Start(
      std::unique_ptr<FakeProducerDelegate>(new FakeProducerDelegate));
#endif

  // Finally, make the consumer connect to the service.
  FakeConsumer consumer(trace_config, std::move(function), &task_runner);
  consumer.Connect(TEST_CONSUMER_SOCK_NAME);

  task_runner.RunUntilCheckpoint("no.more.packets");
}

}  // namespace perfetto
