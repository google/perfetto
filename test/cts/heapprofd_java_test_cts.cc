/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/android_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/android_test_utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/profiling/java_hprof_config.gen.h"
#include "protos/perfetto/trace/profiling/heap_graph.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

// Even though ART is a mainline module, there are dependencies on perfetto for
// OOM heap dumps to work correctly.
bool SupportsOomHeapDump() {
  auto sdk = base::StringToInt32(base::GetAndroidProp("ro.build.version.sdk"));
  if (sdk && *sdk >= 34) {
    PERFETTO_LOG("SDK supports OOME heap dumps");
    return true;
  }
  if (base::GetAndroidProp("ro.build.version.codename") == "UpsideDownCake") {
    PERFETTO_LOG("Codename supports OOME heap dumps");
    return true;
  }
  PERFETTO_LOG("OOME heap dumps not supported");
  return false;
}

std::string RandomSessionName() {
  std::random_device rd;
  std::default_random_engine generator(rd());
  std::uniform_int_distribution<> distribution('a', 'z');

  constexpr size_t kSessionNameLen = 20;
  std::string result(kSessionNameLen, '\0');
  for (size_t i = 0; i < kSessionNameLen; ++i)
    result[i] = static_cast<char>(distribution(generator));
  return result;
}

std::vector<protos::gen::TracePacket> ProfileRuntime(std::string app_name) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }
  StartAppActivity(app_name, "NoopActivity", "target.app.running", &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);
  // If we try to dump too early in app initialization, we sometimes deadlock.
  sleep(1);

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(40 * 1024);
  trace_config.set_duration_ms(3000);
  trace_config.set_data_source_stop_timeout_ms(20000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.java_hprof");
  ds_config->set_target_buffer(0);

  protos::gen::JavaHprofConfig java_hprof_config;
  java_hprof_config.add_process_cmdline(app_name.c_str());
  ds_config->set_java_hprof_config_raw(java_hprof_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name, "new.app.stopped", &task_runner);
  task_runner.RunUntilCheckpoint("new.app.stopped", 10000 /*ms*/);
  return helper.trace();
}

std::vector<protos::gen::TracePacket> TriggerOomHeapDump(
    std::string app_name,
    std::string heap_dump_target) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(40 * 1024);
  trace_config.set_unique_session_name(RandomSessionName().c_str());
  trace_config.set_data_source_stop_timeout_ms(60000);

  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(
      perfetto::protos::gen::TraceConfig::TriggerConfig::START_TRACING);
  trigger_config->set_trigger_timeout_ms(60000);
  auto* oom_trigger = trigger_config->add_triggers();
  oom_trigger->set_name("com.android.telemetry.art-outofmemory");
  oom_trigger->set_stop_delay_ms(1000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.java_hprof.oom");
  ds_config->set_target_buffer(0);

  protos::gen::JavaHprofConfig java_hprof_config;
  java_hprof_config.add_process_cmdline(heap_dump_target.c_str());
  ds_config->set_java_hprof_config_raw(java_hprof_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);
  StartAppActivity(app_name, "JavaOomActivity", "target.app.running",
                   &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);

  if (SupportsOomHeapDump()) {
    helper.WaitForTracingDisabled();
    helper.ReadData();
    helper.WaitForReadData();
  }

  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name, "new.app.stopped", &task_runner);
  task_runner.RunUntilCheckpoint("new.app.stopped", 10000 /*ms*/);
  return helper.trace();
}

void AssertGraphPresent(std::vector<protos::gen::TracePacket> packets) {
  ASSERT_GT(packets.size(), 0u);

  size_t objects = 0;
  size_t roots = 0;
  for (const auto& packet : packets) {
    objects += static_cast<size_t>(packet.heap_graph().objects_size());
    roots += static_cast<size_t>(packet.heap_graph().roots_size());
  }
  ASSERT_GT(objects, 0u);
  ASSERT_GT(roots, 0u);
}

void AssertNoProfileContents(std::vector<protos::gen::TracePacket> packets) {
  // If profile packets are present, they must be empty.
  for (const auto& packet : packets) {
    ASSERT_EQ(packet.heap_graph().roots_size(), 0);
    ASSERT_EQ(packet.heap_graph().objects_size(), 0);
    ASSERT_EQ(packet.heap_graph().types_size(), 0);
    ASSERT_EQ(packet.heap_graph().field_names_size(), 0);
  }
}

TEST(HeapprofdJavaCtsTest, DebuggableAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileRuntime(app_name);
  AssertGraphPresent(packets);
}

TEST(HeapprofdJavaCtsTest, ProfileableAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = ProfileRuntime(app_name);
  AssertGraphPresent(packets);
}

TEST(HeapprofdJavaCtsTest, ReleaseAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = ProfileRuntime(app_name);

  if (!IsUserBuild())
    AssertGraphPresent(packets);
  else
    AssertNoProfileContents(packets);
}

TEST(HeapprofdJavaCtsTest, DebuggableAppRuntimeByPid) {
  std::string app_name = "android.perfetto.cts.app.debuggable";

  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }
  StartAppActivity(app_name, "NoopActivity", "target.app.running", &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);
  // If we try to dump too early in app initialization, we sometimes deadlock.
  sleep(1);

  int target_pid = PidForProcessName(app_name);
  ASSERT_NE(target_pid, -1);

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(40 * 1024);
  trace_config.set_duration_ms(3000);
  trace_config.set_data_source_stop_timeout_ms(20000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.java_hprof");
  ds_config->set_target_buffer(0);

  protos::gen::JavaHprofConfig java_hprof_config;
  java_hprof_config.add_pid(static_cast<uint64_t>(target_pid));
  ds_config->set_java_hprof_config_raw(java_hprof_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name, "new.app.stopped", &task_runner);
  task_runner.RunUntilCheckpoint("new.app.stopped", 10000 /*ms*/);

  const auto& packets = helper.trace();
  AssertGraphPresent(packets);
}

TEST(HeapprofdJavaCtsTest, DebuggableAppOom) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = TriggerOomHeapDump(app_name, "*");
  if (SupportsOomHeapDump()) {
    AssertGraphPresent(packets);
  }
}

TEST(HeapprofdJavaCtsTest, ProfileableAppOom) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = TriggerOomHeapDump(app_name, "*");
  if (SupportsOomHeapDump()) {
    AssertGraphPresent(packets);
  }
}

TEST(HeapprofdJavaCtsTest, ReleaseAppOom) {
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = TriggerOomHeapDump(app_name, "*");
  if (IsUserBuild()) {
    AssertNoProfileContents(packets);
  } else if (SupportsOomHeapDump()) {
    AssertGraphPresent(packets);
  }
}

TEST(HeapprofdJavaCtsTest, DebuggableAppOomNotSelected) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = TriggerOomHeapDump(app_name, "not.this.app");
  AssertNoProfileContents(packets);
}

}  // namespace
}  // namespace perfetto
