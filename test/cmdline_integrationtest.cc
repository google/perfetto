/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <functional>
#include <initializer_list>
#include <thread>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flags.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/tracing_service_state.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "src/perfetto_cmd/bugreport_path.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.gen.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Property;
using ::testing::SizeIs;

std::string RandomTraceFileName() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  constexpr char kSysTmpPath[] = "/data/misc/perfetto-traces";
#else
  constexpr char kSysTmpPath[] = "/tmp";
#endif
  static int suffix = 0;

  std::string path;
  path.assign(kSysTmpPath);
  path.append("/trace-");
  path.append(std::to_string(base::GetBootTimeNs().count()));
  path.append("-");
  path.append(std::to_string(suffix++));
  return path;
}

// For the SaveForBugreport* tests.
TraceConfig CreateTraceConfigForBugreportTest(int score = 1,
                                              bool add_filter = false,
                                              uint32_t msg_count = 3,
                                              uint32_t msg_size = 10) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32768);
  trace_config.set_duration_ms(60000);  // Will never hit this.
  trace_config.set_bugreport_score(score);

  if (add_filter) {
    // Add a trace filter which disallows the trace config echo-back.
    protozero::FilterBytecodeGenerator filt;
    filt.AddNestedField(1 /* root trace.packet*/, 1);
    filt.EndMessage();
    // Add a random unrelated field to keep the generator happy.
    filt.AddSimpleField(protos::pbzero::TracePacket::kTraceUuidFieldNumber);
    filt.EndMessage();
    trace_config.mutable_trace_filter()->set_bytecode_v2(
        filt.Serialize().bytecode);
  }

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");

  ds_config->mutable_for_testing()->set_message_count(msg_count);
  ds_config->mutable_for_testing()->set_message_size(msg_size);
  return trace_config;
}

// For the regular tests.
TraceConfig CreateTraceConfigForTest(uint32_t test_msg_count = 11,
                                     uint32_t test_msg_size = 32) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(test_msg_count);
  ds_config->mutable_for_testing()->set_message_size(test_msg_size);
  return trace_config;
}

void ExpectTraceContainsTestMessages(const protos::gen::Trace& trace,
                                     uint32_t count) {
  ssize_t actual_test_packets_count = std::count_if(
      trace.packet().begin(), trace.packet().end(),
      [](const protos::gen::TracePacket& tp) { return tp.has_for_testing(); });
  EXPECT_EQ(count, static_cast<uint32_t>(actual_test_packets_count));
}

void ExpectTraceContainsTestMessagesWithSize(const protos::gen::Trace& trace,
                                             uint32_t message_size) {
  for (const auto& packet : trace.packet()) {
    if (packet.has_for_testing()) {
      EXPECT_EQ(message_size, packet.for_testing().str().size());
    }
  }
}

void ExpectTraceContainsConfigWithTriggerMode(
    const protos::gen::Trace& trace,
    protos::gen::TraceConfig::TriggerConfig::TriggerMode trigger_mode) {
  // GTest three level nested Property matcher is hard to read, so we use
  // 'find_if' with lambda to ensure the trace config properly includes the
  // trigger mode we set.
  auto found =
      std::find_if(trace.packet().begin(), trace.packet().end(),
                   [trigger_mode](const protos::gen::TracePacket& tp) {
                     return tp.has_trace_config() &&
                            tp.trace_config().trigger_config().trigger_mode() ==
                                trigger_mode;
                   });
  EXPECT_NE(found, trace.packet().end())
      << "Trace config doesn't include expected trigger mode.";
}

class ScopedFileRemove {
 public:
  explicit ScopedFileRemove(const std::string& path) : path_(path) {}
  ~ScopedFileRemove() { remove(path_.c_str()); }
  std::string path_;
};

bool ParseNotEmptyTraceFromFile(const std::string& trace_path,
                                protos::gen::Trace& out) {
  std::string trace_str;
  if (!base::ReadFile(trace_path, &trace_str))
    return false;
  if (trace_str.empty())
    return false;
  return out.ParseFromString(trace_str);
}

std::vector<std::string> GetReceivedTriggerNames(
    const protos::gen::Trace& trace) {
  std::vector<std::string> triggers;
  for (const protos::gen::TracePacket& packet : trace.packet()) {
    if (packet.has_trigger()) {
      triggers.push_back(packet.trigger().trigger_name());
    }
  }
  return triggers;
}

class PerfettoCmdlineTest : public ::testing::Test {
 public:
  void StartServiceIfRequiredNoNewExecsAfterThis() {
    exec_allowed_ = false;
    test_helper_.StartServiceIfRequired();
  }

  TestHelper& test_helper() { return test_helper_; }

  // Creates a process that represents the perfetto binary that will
  // start when Run() is called. |args| will be passed as part of
  // the command line and |std_in| will be piped into std::cin.
  Exec ExecPerfetto(std::initializer_list<std::string> args,
                    std::string std_in = "") {
    // You can not fork after you've started the service due to risk of
    // deadlocks.
    PERFETTO_CHECK(exec_allowed_);
    return Exec("perfetto", std::move(args), std::move(std_in));
  }

  // Creates a process that represents the trigger_perfetto binary that will
  // start when Run() is called. |args| will be passed as part of
  // the command line and |std_in| will be piped into std::cin.
  Exec ExecTrigger(std::initializer_list<std::string> args,
                   std::string std_in = "") {
    // You can not fork after you've started the service due to risk of
    // deadlocks.
    PERFETTO_CHECK(exec_allowed_);
    return Exec("trigger_perfetto", std::move(args), std::move(std_in));
  }

  // This is in common to the 3 TEST_F SaveForBugreport* fixtures, which differ
  // only in the config, passed here as input.
  void RunBugreportTest(protos::gen::TraceConfig trace_config,
                        bool check_original_trace = true,
                        bool use_explicit_clone = false) {
    const std::string path = RandomTraceFileName();
    ScopedFileRemove remove_on_test_exit(path);

    auto perfetto_proc = ExecPerfetto(
        {
            "-o",
            path,
            "-c",
            "-",
        },
        trace_config.SerializeAsString());

    Exec perfetto_br_proc =
        use_explicit_clone
            ? ExecPerfetto({"--out", GetBugreportTracePath(), "--clone", "-1"})
            : ExecPerfetto({"--save-for-bugreport"});

    // Start the service and connect a simple fake producer.
    StartServiceIfRequiredNoNewExecsAfterThis();

    auto* fake_producer = test_helper().ConnectFakeProducer();
    ASSERT_TRUE(fake_producer);

    std::thread background_trace([&perfetto_proc]() {
      std::string stderr_str;
      ASSERT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
    });

    // Wait for the producer to start, and then write out packets.
    test_helper().WaitForProducerEnabled();
    auto on_data_written = task_runner_.CreateCheckpoint("data_written");
    fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
    task_runner_.RunUntilCheckpoint("data_written");

    ASSERT_EQ(0, perfetto_br_proc.Run(&stderr_)) << "stderr: " << stderr_;
    perfetto_proc.SendSigterm();
    background_trace.join();

    uint32_t expected_packets = 0;
    for (auto& ds : trace_config.data_sources()) {
      if (ds.config().has_for_testing())
        expected_packets = ds.config().for_testing().message_count();
    }

    auto check_trace_contents = [expected_packets](std::string trace_path) {
      // Read the trace written in the fixed location
      // (/data/misc/perfetto-traces/ on Android, /tmp/ on Linux/Mac) and make
      // sure it has the right contents.
      protos::gen::Trace trace;
      ASSERT_TRUE(ParseNotEmptyTraceFromFile(trace_path, trace));
      uint32_t test_packets = 0;
      for (const auto& p : trace.packet())
        test_packets += p.has_for_testing() ? 1 : 0;
      ASSERT_EQ(test_packets, expected_packets) << trace_path;
    };

    // Verify that both the original trace and the cloned bugreport contain
    // the expected contents.
    check_trace_contents(GetBugreportTracePath());
    if (check_original_trace)
      check_trace_contents(path);
  }

  // Tests are allowed to freely use these variables.
  std::string stderr_;
  base::TestTaskRunner task_runner_;

  // We use these two constants to set test data payload parameters and assert
  // it was correctly written to the trace.
  static constexpr size_t kTestMessageCount = 11;
  static constexpr size_t kTestMessageSize = 32;

 private:
  bool exec_allowed_ = true;
  TestHelper test_helper_{&task_runner_};
};

}  // namespace

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define AndroidOnly(x) x
#else
#define AndroidOnly(x) DISABLED_##x
#endif

TEST_F(PerfettoCmdlineTest, InvalidCases) {
  std::string cfg("duration_ms: 100");

  auto invalid_arg = ExecPerfetto({"--invalid-arg"});
  auto empty_config = ExecPerfetto({"-c", "-", "-o", "-"}, "");

  // Cannot make assertions on --dropbox because on standalone builds it fails
  // prematurely due to lack of dropbox.
  auto missing_dropbox =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--dropbox=foo"}, cfg);
  auto either_out_or_dropbox = ExecPerfetto({"-c", "-", "--txt"}, cfg);

  // Disallow mixing simple and file config.
  auto simple_and_file_1 =
      ExecPerfetto({"-o", "-", "-c", "-", "-t", "2s"}, cfg);
  auto simple_and_file_2 =
      ExecPerfetto({"-o", "-", "-c", "-", "-b", "2m"}, cfg);
  auto simple_and_file_3 =
      ExecPerfetto({"-o", "-", "-c", "-", "-s", "2m"}, cfg);

  // Invalid --attach / --detach cases.
  auto invalid_stop =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--stop"}, cfg);
  auto attach_and_config_1 =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--attach=foo"}, cfg);
  auto attach_and_config_2 =
      ExecPerfetto({"-t", "2s", "-o", "-", "--attach=foo"}, cfg);
  auto attach_needs_argument = ExecPerfetto({"--attach"}, cfg);
  auto detach_needs_argument =
      ExecPerfetto({"-t", "2s", "-o", "-", "--detach"}, cfg);
  auto detach_without_out_or_dropbox =
      ExecPerfetto({"-t", "2s", "--detach=foo"}, cfg);

  // Cannot trace and use --query.
  auto trace_and_query_1 = ExecPerfetto({"-t", "2s", "--query"}, cfg);
  auto trace_and_query_2 = ExecPerfetto({"-c", "-", "--query"}, cfg);

  // Ensure all Exec:: calls have been saved to prevent deadlocks.
  StartServiceIfRequiredNoNewExecsAfterThis();

  EXPECT_EQ(1, invalid_arg.Run(&stderr_));

  EXPECT_EQ(1, empty_config.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("TraceConfig is empty"));

  // Cannot make assertions on --upload because on standalone builds it fails
  // prematurely due to lack of dropbox.
  EXPECT_EQ(1, missing_dropbox.Run(&stderr_));

  EXPECT_EQ(1, either_out_or_dropbox.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Either --out or --upload"));

  // Disallow mixing simple and file config.
  EXPECT_EQ(1, simple_and_file_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, simple_and_file_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, simple_and_file_3.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  // Invalid --attach / --detach cases.
  EXPECT_EQ(1, invalid_stop.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("--stop is supported only in combination"));

  EXPECT_EQ(1, attach_and_config_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, attach_and_config_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, attach_needs_argument.Run(&stderr_));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--attach.*requires an argument"));

  EXPECT_EQ(1, detach_needs_argument.Run(&stderr_));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--detach.*requires an argument"));

  EXPECT_EQ(1, detach_without_out_or_dropbox.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("--out or --upload is required"));

  // Cannot trace and use --query.
  EXPECT_EQ(1, trace_and_query_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, trace_and_query_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));
}

TEST_F(PerfettoCmdlineTest, Version) {
  auto perfetto = ExecPerfetto({"--version"});
  EXPECT_EQ(0, perfetto.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, TxtConfig) {
  std::string cfg("duration_ms: 100");
  auto perfetto = ExecPerfetto({"-c", "-", "--txt", "-o", "-"}, cfg);
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, perfetto.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, SimpleConfig) {
  auto perfetto = ExecPerfetto({"-o", "-", "-c", "-", "-t", "100ms"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, perfetto.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, DetachAndAttach) {
  auto attach_to_not_existing = ExecPerfetto({"--attach=not_existent"});

  std::string cfg("duration_ms: 10000; write_into_file: true");
  auto detach_valid_stop =
      ExecPerfetto({"-o", "-", "-c", "-", "--txt", "--detach=valid_stop"}, cfg);
  auto stop_valid_stop = ExecPerfetto({"--attach=valid_stop", "--stop"});

  StartServiceIfRequiredNoNewExecsAfterThis();

  EXPECT_NE(0, attach_to_not_existing.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Session re-attach failed"));

  EXPECT_EQ(0, detach_valid_stop.Run(&stderr_)) << stderr_;
  EXPECT_EQ(0, stop_valid_stop.Run(&stderr_));
}

TEST_F(PerfettoCmdlineTest, StartTracingTrigger) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::START_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  auto trigger_proc = ExecTrigger({"trigger_name"});

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();

  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  // Start a background thread that will deliver the config now that we've
  // started the service. See |perfetto_proc| above for the args passed.
  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerSetup();
  EXPECT_EQ(0, trigger_proc.Run(&stderr_));

  // Wait for the producer to start, and then write out some test packets.
  test_helper().WaitForProducerEnabled();
  auto on_data_written = task_runner_.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written");
  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path, trace));
  ExpectTraceContainsConfigWithTriggerMode(
      trace, protos::gen::TraceConfig::TriggerConfig::START_TRACING);
  EXPECT_THAT(GetReceivedTriggerNames(trace), ElementsAre("trigger_name"));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
}

TEST_F(PerfettoCmdlineTest, StopTracingTrigger) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  auto trigger_proc =
      ExecTrigger({"trigger_name_2", "trigger_name", "trigger_name_3"});

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  // Start a background thread that will deliver the config now that we've
  // started the service. See |perfetto_proc| above for the args passed.
  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  // Wait for the producer to start, and then write out some test packets,
  // before the trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, trigger_proc.Run(&stderr_)) << "stderr: " << stderr_;

  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path, trace));
  ExpectTraceContainsConfigWithTriggerMode(
      trace, protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  EXPECT_THAT(GetReceivedTriggerNames(trace),
              ElementsAre("trigger_name", "trigger_name_3"));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
}

// Dropbox on the commandline client only works on android builds. So disable
// this test on all other builds.
TEST_F(PerfettoCmdlineTest, AndroidOnly(NoDataNoFileWithoutTrigger)) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* incident_config = trace_config.mutable_incident_report_config();
  incident_config->set_destination_package("foo.bar.baz");
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(1000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "--dropbox",
          "TAG",
          "--no-guardrails",
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::string stderr_str;
  std::thread background_trace([&perfetto_proc, &stderr_str]() {
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str));
  });
  background_trace.join();

  EXPECT_THAT(stderr_str,
              ::testing::HasSubstr("Skipping write to incident. Empty trace."));
}

TEST_F(PerfettoCmdlineTest, StopTracingTriggerFromConfig) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  std::string triggers = R"(
    activate_triggers: "trigger_name_2"
    activate_triggers: "trigger_name"
    activate_triggers: "trigger_name_3"
  )";
  auto perfetto_proc_2 = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  // Wait for the producer to start, and then write out some test packets,
  // before the trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, perfetto_proc_2.Run(&stderr_)) << "stderr: " << stderr_;

  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path, trace));
  EXPECT_LT(static_cast<int>(kTestMessageCount), trace.packet_size());
  ExpectTraceContainsConfigWithTriggerMode(
      trace, protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  EXPECT_THAT(GetReceivedTriggerNames(trace),
              ElementsAre("trigger_name", "trigger_name_3"));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
}

TEST_F(PerfettoCmdlineTest, TriggerFromConfigStopsFileOpening) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  std::string triggers = R"(
    activate_triggers: "trigger_name_2"
    activate_triggers: "trigger_name"
    activate_triggers: "trigger_name_3"
  )";
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::string trace_str;
  EXPECT_FALSE(base::ReadFile(path, &trace_str));

  EXPECT_EQ(0, perfetto_proc.Run(&stderr_)) << "stderr: " << stderr_;

  EXPECT_FALSE(base::ReadFile(path, &trace_str));
}

TEST_F(PerfettoCmdlineTest, Query) {
  auto query = ExecPerfetto({"--query"});
  auto query_raw = ExecPerfetto({"--query-raw"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, query.Run(&stderr_)) << stderr_;
  EXPECT_EQ(0, query_raw.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, AndroidOnly(CmdTriggerWithUploadFlag)) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  std::string triggers = R"(
    activate_triggers: "trigger_name"
  )";
  auto perfetto_proc_2 = ExecPerfetto(
      {
          "--upload",
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  // Wait for the producer to start, and then write out some test packets,
  // before the trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, perfetto_proc_2.Run(&stderr_)) << "stderr: " << stderr_;

  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path, trace));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
  EXPECT_LT(static_cast<int>(kTestMessageCount), trace.packet_size());
  EXPECT_THAT(trace.packet(),
              Contains(Property(&protos::gen::TracePacket::trigger,
                                Property(&protos::gen::Trigger::trigger_name,
                                         Eq("trigger_name")))));
}

TEST_F(PerfettoCmdlineTest, TriggerCloneSnapshot) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::CLONE_SNAPSHOT);
  trigger_cfg->set_trigger_timeout_ms(600000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  std::string triggers = R"(
    activate_triggers: "trigger_name"
  )";
  auto trigger_proc = ExecPerfetto(
      {
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  // Wait for the producer to start, and then write out some test packets,
  // before the trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, trigger_proc.Run(&stderr_)) << "stderr: " << stderr_;

  // Now we need to wait that the `perfetto_proc` creates the snapshot trace
  // file in the trace/path.0 file (appending .0). Once that is done we can
  // kill the perfetto cmd (otherwise it will keep running for the whole
  // trigger_timeout_ms, unlike the case of STOP_TRACING.
  std::string snapshot_path = path + ".0";
  for (int i = 0; i < 100 && !base::FileExists(snapshot_path); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(base::FileExists(snapshot_path));

  perfetto_proc.SendSigterm();
  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(snapshot_path, trace));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
  EXPECT_LT(static_cast<int>(kTestMessageCount), trace.packet_size());
  EXPECT_THAT(trace.packet(),
              Contains(Property(&protos::gen::TracePacket::trigger,
                                Property(&protos::gen::Trigger::trigger_name,
                                         Eq("trigger_name")))));
}

TEST_F(PerfettoCmdlineTest, MultipleTriggersCloneSnapshot) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::CLONE_SNAPSHOT);
  trigger_cfg->set_trigger_timeout_ms(600000);
  // Add two triggers, the "trigger_name_2" hits before "trigger_name_1".
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_1");
  trigger->set_stop_delay_ms(1500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_2");
  trigger->set_stop_delay_ms(500);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  auto triggers_proc = ExecTrigger({"trigger_name_1", "trigger_name_2"});

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  // Wait for the producer to start, and then write out some test packets,
  // before the trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, triggers_proc.Run(&stderr_)) << "stderr: " << stderr_;

  // Wait for both clone triggers to hit and wait for two snapshot files.
  std::string snapshot_path = path + ".0";
  for (int i = 0; i < 100 && !base::FileExists(snapshot_path); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(base::FileExists(snapshot_path));

  std::string snapshot_path_2 = path + ".1";
  for (int i = 0; i < 100 && !base::FileExists(snapshot_path_2); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(base::FileExists(snapshot_path_2));

  perfetto_proc.SendSigterm();
  background_trace.join();

  // We now have two traces, the first one was cloned by "trigger_name_2",
  // the second was cloned by "trigger_name_1".

  // Asserts for the first trace.
  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(snapshot_path, trace));
  EXPECT_LT(static_cast<int>(kTestMessageCount), trace.packet_size());
  EXPECT_THAT(GetReceivedTriggerNames(trace),
              ElementsAre("trigger_name_1", "trigger_name_2"));

  std::vector<protos::gen::TracePacket> clone_trigger_packets;
  protos::gen::TracePacket trigger_packet;
  for (const auto& packet : trace.packet()) {
    if (packet.has_clone_snapshot_trigger()) {
      clone_trigger_packets.push_back(packet);
    } else if (packet.has_trigger() &&
               packet.trigger().trigger_name() == "trigger_name_2") {
      trigger_packet = packet;
    }
  }
  ASSERT_EQ(clone_trigger_packets.size(), 1ul);
  EXPECT_EQ(clone_trigger_packets[0].clone_snapshot_trigger().trigger_name(),
            "trigger_name_2");
  EXPECT_EQ(clone_trigger_packets[0].clone_snapshot_trigger().stop_delay_ms(),
            500u);
  // Assert that all fields of 'clone_snapshot_trigger' equal to the same fields
  // of a 'trigger'.
  EXPECT_EQ(clone_trigger_packets[0].timestamp(), trigger_packet.timestamp());
  EXPECT_EQ(clone_trigger_packets[0].clone_snapshot_trigger(),
            trigger_packet.trigger());

  // Asserts for the second trace.
  protos::gen::Trace trace_2;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(snapshot_path_2, trace_2));
  EXPECT_LT(static_cast<int>(kTestMessageCount), trace_2.packet_size());
  // List of received triggers from the main session was cleaned after the first
  // clone operation happened, the list is empty in the second trace.
  EXPECT_THAT(GetReceivedTriggerNames(trace_2), IsEmpty());

  std::vector<protos::gen::TracePacket> clone_trigger_packets_2;
  for (const auto& packet : trace_2.packet()) {
    if (packet.has_clone_snapshot_trigger()) {
      clone_trigger_packets_2.push_back(packet);
    }
  }
  ASSERT_EQ(clone_trigger_packets_2.size(), 1ul);
  EXPECT_EQ(clone_trigger_packets_2[0].clone_snapshot_trigger().trigger_name(),
            "trigger_name_1");
  EXPECT_EQ(clone_trigger_packets_2[0].clone_snapshot_trigger().stop_delay_ms(),
            1500u);

  // There is no triggers in the second snapshot, but we can compare the
  // "clone_snapshot_trigger" with the trigger saved into the first snapshot.
  protos::gen::TracePacket trigger_packet_from_first_snapshot;
  for (const auto& packet : trace.packet()) {
    if (packet.has_trigger() &&
        packet.trigger().trigger_name() == "trigger_name_1") {
      trigger_packet_from_first_snapshot = packet;
    }
  }
  // Assert that all fields of 'clone_snapshot_trigger' equal to the same fields
  // of a 'trigger'.
  EXPECT_EQ(clone_trigger_packets_2[0].timestamp(),
            trigger_packet_from_first_snapshot.timestamp());
  EXPECT_EQ(clone_trigger_packets_2[0].clone_snapshot_trigger(),
            trigger_packet_from_first_snapshot.trigger());
}

TEST_F(PerfettoCmdlineTest, SaveForBugreport) {
  TraceConfig trace_config = CreateTraceConfigForBugreportTest();
  RunBugreportTest(std::move(trace_config));
}

TEST_F(PerfettoCmdlineTest, SaveForBugreport_WriteIntoFile) {
  TraceConfig trace_config = CreateTraceConfigForBugreportTest();
  trace_config.set_file_write_period_ms(60000);  // Will never hit this.
  trace_config.set_write_into_file(true);
  RunBugreportTest(std::move(trace_config));
}

TEST_F(PerfettoCmdlineTest, Clone) {
  TraceConfig trace_config = CreateTraceConfigForBugreportTest();
  RunBugreportTest(std::move(trace_config), /*check_original_trace=*/true,
                   /*use_explicit_clone=*/true);
}

TEST_F(PerfettoCmdlineTest, CloneByName) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  trace_config.set_unique_session_name("my_unique_session_name");

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  const std::string path_cloned = RandomTraceFileName();
  ScopedFileRemove path_cloned_remove(path_cloned);
  auto perfetto_proc_clone = ExecPerfetto({
      "-o",
      path_cloned,
      "--clone-by-name",
      "my_unique_session_name",
  });

  const std::string path_cloned_2 = RandomTraceFileName();
  ScopedFileRemove path_cloned_2_remove(path_cloned_2);
  auto perfetto_proc_clone_2 = ExecPerfetto({
      "-o",
      path_cloned_2,
      "--clone-by-name",
      "non_existing_session_name",
  });

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();

  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, perfetto_proc_clone.Run(&stderr_)) << "stderr: " << stderr_;
  EXPECT_TRUE(base::FileExists(path_cloned));

  // The command still returns 0, but doesn't create a file.
  EXPECT_EQ(0, perfetto_proc_clone_2.Run(&stderr_)) << "stderr: " << stderr_;
  EXPECT_FALSE(base::FileExists(path_cloned_2));

  protos::gen::Trace cloned_trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path_cloned, cloned_trace));
  ExpectTraceContainsTestMessages(cloned_trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(cloned_trace, kTestMessageSize);

  perfetto_proc.SendSigterm();
  background_trace.join();

  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(path, trace));
  ExpectTraceContainsTestMessages(trace, kTestMessageCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
}

TEST_F(PerfettoCmdlineTest, CloneWriteIntoFileSession) {
  protos::gen::TraceConfig trace_config =
      CreateTraceConfigForTest(kTestMessageCount, kTestMessageSize);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(10);
  trace_config.set_unique_session_name("my_session_name");

  const std::string write_into_file_path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(write_into_file_path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          write_into_file_path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  const std::string cloned_file_path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit_1(cloned_file_path);
  auto clone_proc = ExecPerfetto(
      {"--out", cloned_file_path, "--clone-by-name", "my_session_name"});

  StartServiceIfRequiredNoNewExecsAfterThis();

  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  test_helper().WaitForProducerEnabled();
  auto on_data_written = task_runner_.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written");

  // Wait until all the data for the 'write_into_file' session is written into
  // file.
  bool write_into_file_data_ready = false;
  for (int i = 0; i < 100; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protos::gen::Trace trace;
    if (!ParseNotEmptyTraceFromFile(write_into_file_path, trace)) {
      continue;
    }
    ssize_t test_packets_count =
        std::count_if(trace.packet().begin(), trace.packet().end(),
                      [](const protos::gen::TracePacket& tp) {
                        return tp.has_for_testing();
                      });
    if (test_packets_count == kTestMessageCount) {
      write_into_file_data_ready = true;
      break;
    }
  }
  ASSERT_TRUE(write_into_file_data_ready);

  // Now we clone the session.
  std::string stderr_str;
  EXPECT_EQ(0, clone_proc.Run(&stderr_str)) << stderr_str;

  perfetto_proc.SendSigterm();
  background_trace.join();
  // And now we assert that both original 'write_into_file' and the cloned
  // session have the same events.
  {
    protos::gen::Trace trace;
    ASSERT_TRUE(ParseNotEmptyTraceFromFile(write_into_file_path, trace));
    ExpectTraceContainsTestMessages(trace, kTestMessageCount);
    ExpectTraceContainsTestMessagesWithSize(trace, kTestMessageSize);
  }
  {
    protos::gen::Trace cloned_trace;
    ASSERT_TRUE(ParseNotEmptyTraceFromFile(cloned_file_path, cloned_trace));
    ExpectTraceContainsTestMessages(cloned_trace, kTestMessageCount);
    ExpectTraceContainsTestMessagesWithSize(cloned_trace, kTestMessageSize);
  }
}

// Regression test for b/279753347: --save-for-bugreport would create an empty
// file if no session with bugreport_score was active.
TEST_F(PerfettoCmdlineTest, UnavailableBugreportLeavesNoEmptyFiles) {
  ScopedFileRemove remove_on_test_exit(GetBugreportTracePath());
  Exec perfetto_br_proc = ExecPerfetto({"--save-for-bugreport"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  perfetto_br_proc.Run(&stderr_);
  // No file exists. Great.
  if (!base::FileExists(GetBugreportTracePath())) {
    return;
  }
  // A file exists. There are two possiblilities:
  // 1. There was a bugreport_score session.
  // 2. There was no bugreport_score session and we're hitting b/279753347.
  //
  // Let's check that we're not hitting b/279753347, by checking that the file
  // is not empty.
  EXPECT_NE(base::GetFileSize(GetBugreportTracePath()), 0);
}

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
TEST_F(PerfettoCmdlineTest, DoNotDeleteNotEmptyWriteIntoFileTraceOnError) {
  // We call `test_helper().RestartService()` to simulate `traced` dropping
  // the connection to the `perfetto_cmd`, so we run this test only in
  // `PERFETTO_START_DAEMONS` mode.
  // FakeProducer crashes is this case, so we just don't start it. Even without
  // a data source, the tracing session writes some data on disk, that is enough
  // for us.
  TraceConfig trace_config;
  trace_config.set_unique_session_name("my_write_into_file_session");
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(10);

  const std::string write_into_file_path = RandomTraceFileName();
  ScopedFileRemove remove_on_test_exit(write_into_file_path);
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          write_into_file_path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  StartServiceIfRequiredNoNewExecsAfterThis();

  std::string perfetto_cmd_stderr;
  std::thread background_trace([&perfetto_proc, &perfetto_cmd_stderr]() {
    EXPECT_EQ(0, perfetto_proc.Run(&perfetto_cmd_stderr))
        << perfetto_cmd_stderr;
  });

  // Wait until some data is written into the trace file.
  bool write_into_file_data_ready = false;
  for (int i = 0; i < 100; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protos::gen::Trace trace;
    if (ParseNotEmptyTraceFromFile(write_into_file_path, trace)) {
      write_into_file_data_ready = true;
      break;
    }
  }
  ASSERT_TRUE(write_into_file_data_ready);

  // Tracing session is still running, now simulate `traced` dropping the
  // connection to the `perfetto_cmd`
  test_helper().RestartService();

  background_trace.join();
  // Assert perfetto_cmd disconnected with an error.
  EXPECT_THAT(
      perfetto_cmd_stderr,
      HasSubstr("Service error: EnableTracing IPC request rejected. This is "
                "likely due to a loss of the traced connection"));
  // Assert trace file exists and not empty.
  protos::gen::Trace trace;
  EXPECT_TRUE(ParseNotEmptyTraceFromFile(write_into_file_path, trace));
}
#endif

// Tests that SaveTraceForBugreport() works also if the trace has triggers
// defined and those triggers have not been hit. This is a regression test for
// b/188008375 .
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
// Disabled due to b/191940560
#define MAYBE_SaveForBugreport_Triggers DISABLED_SaveForBugreport_Triggers
#else
#define MAYBE_SaveForBugreport_Triggers SaveForBugreport_Triggers
#endif
TEST_F(PerfettoCmdlineTest, MAYBE_SaveForBugreport_Triggers) {
  TraceConfig trace_config = CreateTraceConfigForBugreportTest();
  trace_config.set_duration_ms(0);  // set_trigger_timeout_ms is used instead.
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_timeout_ms(8.64e+7);
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  RunBugreportTest(std::move(trace_config), /*check_original_trace=*/false);
}

TEST_F(PerfettoCmdlineTest, SaveAllForBugreport_NoTraces) {
  auto save_all_cmd = ExecPerfetto({"--save-all-for-bugreport"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, save_all_cmd.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("No tracing sessions eligible"));
}

TEST_F(PerfettoCmdlineTest, SaveAllForBugreport_FourTraces) {
  struct TraceProc {
    explicit TraceProc(TraceConfig c) : cfg(std::move(c)) {}

    TraceConfig cfg;
    std::optional<Exec> proc;
    std::thread thd;
  };

  auto remove_br_files = [] {
    remove((GetBugreportTraceDir() + "/systrace.pftrace").c_str());
    remove((GetBugreportTraceDir() + "/custom_name.pftrace").c_str());
    remove((GetBugreportTraceDir() + "/custom_name_1.pftrace").c_str());
    remove((GetBugreportTraceDir() + "/systrace_1.pftrace").c_str());
  };

  remove_br_files();  // Remove both before and after ending the test.
  auto remove_on_exit = base::OnScopeExit(remove_br_files);

  auto session_prefix = "bugreport_test_" +
                        std::to_string(base::GetWallTimeNs().count() % 1000000);

  // Create four tracing sessions with different bugreport scores.
  // Two of them will have the default "systrace.pftrace" name.
  std::vector<TraceProc> traces;
  const bool add_filt = true;
  traces.emplace_back(CreateTraceConfigForBugreportTest(/*score=*/1, add_filt));
  traces.back().cfg.set_unique_session_name(session_prefix + "_1");

  traces.emplace_back(CreateTraceConfigForBugreportTest(/*score=*/2, add_filt));
  traces.back().cfg.set_bugreport_filename("custom_name.pftrace");
  traces.back().cfg.set_unique_session_name(session_prefix + "_2");

  traces.emplace_back(CreateTraceConfigForBugreportTest(/*score=*/3, add_filt));
  traces.back().cfg.set_bugreport_filename("custom_name.pftrace");
  traces.back().cfg.set_unique_session_name(session_prefix + "_3");

  traces.emplace_back(CreateTraceConfigForBugreportTest(/*score=*/4, add_filt));
  traces.back().cfg.set_unique_session_name(session_prefix + "_4");

  for (auto& trace : traces) {
    std::string cfg = trace.cfg.SerializeAsString();
    trace.proc = ExecPerfetto({"-o", base::kDevNull, "-c", "-"}, cfg);
  }

  Exec perfetto_br_proc = ExecPerfetto({"--save-all-for-bugreport"});

  StartServiceIfRequiredNoNewExecsAfterThis();

  for (auto& trace : traces) {
    trace.thd = std::thread([&trace] {
      std::string stderr_str;
      ASSERT_EQ(0, trace.proc->Run(&stderr_str)) << stderr_str;
      PERFETTO_DLOG("perfetto-cmd output:\n%s", stderr_str.c_str());
    });
  }

  // Wait that all tracing sessions are started.
  // Note that in CTS mode, the Android test infra will start other tracing
  // sessions for performance reasons. We can't just wait to see 4 sessions,
  // we need to actually check the unique session name.
  test_helper().ConnectConsumer();
  test_helper().WaitForConsumerConnect();
  for (;;) {
    auto state = test_helper().QueryServiceStateAndWait();
    const auto& sessions = state.tracing_sessions();
    if (std::count_if(sessions.begin(), sessions.end(),
                      [&](const TracingServiceState::TracingSession& s) {
                        return base::StartsWith(s.unique_session_name(),
                                                session_prefix);
                      }) >= 4) {
      break;
    }
    base::SleepMicroseconds(100 * 1000);
  }

  EXPECT_EQ(0, perfetto_br_proc.Run(&stderr_)) << stderr_;
  PERFETTO_DLOG("perfetto --save-all-for-bugreport output:\n-----\n%s\n-----\n",
                stderr_.c_str());

  // Stop all the four ongoing traces, which by now got cloned.
  for (auto& trace : traces) {
    trace.proc->SendSigterm();
    trace.thd.join();
  }

  auto check_trace = [&](std::string fname, int expected_score) {
    std::string fpath = GetBugreportTraceDir() + "/" + fname;
    ASSERT_TRUE(base::FileExists(fpath)) << fpath;
    protos::gen::Trace trace;
    ASSERT_TRUE(ParseNotEmptyTraceFromFile(fpath, trace)) << fpath;
    EXPECT_THAT(
        trace.packet(),
        Contains(Property(&protos::gen::TracePacket::trace_config,
                          Property(&protos::gen::TraceConfig::bugreport_score,
                                   Eq(expected_score)))));
  };

  check_trace("systrace.pftrace", /*expected_score=*/4);
  check_trace("custom_name.pftrace", /*expected_score=*/3);
  check_trace("custom_name_1.pftrace", /*expected_score=*/2);
  check_trace("systrace_1.pftrace", /*expected_score=*/1);
}

TEST_F(PerfettoCmdlineTest, SaveAllForBugreport_LargeTrace) {
  auto remove_br_files = [] {
    remove((GetBugreportTraceDir() + "/systrace.pftrace").c_str());
  };

  remove_br_files();  // Remove both before and after ending the test.
  auto remove_on_exit = base::OnScopeExit(remove_br_files);

  const uint32_t kMsgCount = 10000;
  const uint32_t kMsgSize = 1024;
  TraceConfig cfg = CreateTraceConfigForBugreportTest(
      /*score=*/1, /*add_filter=*/false, kMsgCount, kMsgSize);

  auto session_name = "bugreport_test_" +
                      std::to_string(base::GetWallTimeNs().count() % 1000000);

  cfg.set_unique_session_name(session_name);
  std::string cfg_str = cfg.SerializeAsString();
  Exec trace_proc = ExecPerfetto({"-o", base::kDevNull, "-c", "-"}, cfg_str);
  Exec perfetto_br_proc = ExecPerfetto({"--save-all-for-bugreport"});

  StartServiceIfRequiredNoNewExecsAfterThis();

  auto* fake_producer = test_helper().ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread thd([&trace_proc] {
    std::string stderr_str;
    ASSERT_EQ(0, trace_proc.Run(&stderr_str)) << stderr_str;
    PERFETTO_DLOG("perfetto-cmd output:\n%s", stderr_str.c_str());
  });

  // Wait that the tracing session is started.
  test_helper().ConnectConsumer();
  test_helper().WaitForConsumerConnect();
  for (;;) {
    auto state = test_helper().QueryServiceStateAndWait();
    const auto& sessions = state.tracing_sessions();
    if (std::count_if(sessions.begin(), sessions.end(),
                      [&](const TracingServiceState::TracingSession& s) {
                        return s.unique_session_name() == session_name;
                      }) >= 1) {
      break;
    }
    base::SleepMicroseconds(100 * 1000);
  }
  test_helper().SyncAndWaitProducer();

  auto on_data_written = task_runner_.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(test_helper().WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written");

  EXPECT_EQ(0, perfetto_br_proc.Run(&stderr_)) << stderr_;
  PERFETTO_DLOG("perfetto --save-all-for-bugreport output:\n-----\n%s\n-----\n",
                stderr_.c_str());

  // Stop the ongoing trace, which by now got cloned.
  trace_proc.SendSigterm();
  thd.join();

  std::string fpath = GetBugreportTraceDir() + "/systrace.pftrace";
  ASSERT_TRUE(base::FileExists(fpath)) << fpath;
  protos::gen::Trace trace;
  ASSERT_TRUE(ParseNotEmptyTraceFromFile(fpath, trace)) << fpath;
  ExpectTraceContainsTestMessages(trace, kMsgCount);
  ExpectTraceContainsTestMessagesWithSize(trace, kMsgSize);
}

}  // namespace perfetto
