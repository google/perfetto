/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/trace_redactor.h"

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "protos/perfetto/trace/android/packages_list.gen.h"
#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/timeline_validation.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {
namespace {

class DummyCollect : public CollectPrimitive {
 public:
  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override {
    if (packet.has_trusted_uid() && !context->package_uid.has_value()) {
      context->package_uid = static_cast<uint64_t>(packet.trusted_uid());
    }
    return base::OkStatus();
  }
};

class DummyValidator : public ValidatorPrimitive {
 public:
  base::Status Validate(const Context&) const override {
    return base::ErrStatus("DummyValidator: validation failed");
  }
};

class DummyBuild : public BuildPrimitive {
 public:
  base::Status Build(Context*) const override { return base::OkStatus(); }
};

class DummyTransform : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override {
    protos::pbzero::TracePacket::Decoder decoder(*packet);
    if (decoder.has_trusted_uid() && context.package_uid.has_value() &&
        static_cast<uint64_t>(decoder.trusted_uid()) != *context.package_uid) {
      packet->clear();
    }
    return base::OkStatus();
  }
};

class DummyAugment : public AugmentPrimitive {
 public:
  base::Status Augment(const Context& context, std::string* packet) override {
    if (emitted_ || !context.package_uid.has_value()) {
      return base::OkStatus();
    }
    emitted_ = true;
    protos::gen::TracePacket gen_packet;
    gen_packet.set_timestamp(999);
    gen_packet.set_trusted_uid(static_cast<int32_t>(*context.package_uid));
    packet->assign(gen_packet.SerializeAsString());
    return base::OkStatus();
  }

 private:
  bool emitted_ = false;
};

template <uint64_t TS>
class DummyAugmentTimestamp : public AugmentPrimitive {
 public:
  base::Status Augment(const Context& context, std::string* packet) override {
    if (emitted_ || !context.package_uid.has_value()) {
      return base::OkStatus();
    }
    emitted_ = true;
    protos::gen::TracePacket gen_packet;
    gen_packet.set_timestamp(TS);
    gen_packet.set_trusted_uid(static_cast<int32_t>(*context.package_uid));
    packet->assign(gen_packet.SerializeAsString());
    return base::OkStatus();
  }

 private:
  bool emitted_ = false;
};

}  // namespace

TEST(TraceRedactorPassTest, DirectPassExecution) {
  protos::gen::Trace trace;
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(100);
    packet->set_trusted_uid(1000);
  }
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(200);
    packet->set_trusted_uid(2000);  // Will be dropped by transform
  }
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(300);
    packet->set_trusted_uid(1000);
  }

  std::string serialized = trace.SerializeAsString();

  TraceRedactorPass pass;
  pass.emplace_collect<DummyCollect>();
  pass.emplace_transform<DummyTransform>();
  pass.emplace_augment<DummyAugment>();

  Context context;
  context.package_uid = 1000;
  std::string output_buffer;
  ASSERT_OK(pass.Redact(serialized, &context, &output_buffer));

  protos::pbzero::Trace::Decoder output_trace(output_buffer);
  std::vector<uint64_t> timestamps;
  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());
    timestamps.push_back(p.timestamp());
  }

  // Packets should be: timestamp 100, timestamp 300, and augmented packet 999.
  ASSERT_EQ(timestamps.size(), 3u);
  EXPECT_EQ(timestamps[0], 100u);
  EXPECT_EQ(timestamps[1], 300u);
  EXPECT_EQ(timestamps[2], 999u);
}

TEST(TraceRedactorPassTest, ValidatorErrorHaltsExecution) {
  protos::gen::Trace trace;
  auto* packet = trace.add_packet();
  packet->set_timestamp(100);

  std::string serialized = trace.SerializeAsString();

  TraceRedactorPass pass;
  pass.emplace_validator<DummyValidator>();

  Context context;
  std::string output_buffer;
  auto status = pass.Redact(serialized, &context, &output_buffer);

  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(), "DummyValidator: validation failed");
}

TEST(TraceRedactorPassTest, EmptyTimelineWithTimelineValidationReturnsError) {
  protos::gen::Trace trace;
  auto* packet = trace.add_packet();
  packet->set_timestamp(100);

  std::string serialized = trace.SerializeAsString();

  TraceRedactorPass pass;
  pass.emplace_validator<TimelineValidation>();

  Context context;  // timeline is null / empty
  std::string output_buffer;
  auto status = pass.Redact(serialized, &context, &output_buffer);

  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(),
            "TraceRedactor: No process timeline found. Are sched_free or "
            "process stats data sources missing");
}

TEST(TraceRedactorTest, EmptyTimelineReturnsError) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  protos::gen::Trace trace;
  auto* packet = trace.add_packet();
  packet->set_trusted_uid(9999);

  auto* packages = packet->mutable_packages_list();
  auto* package = packages->add_packages();
  package->set_uid(1037);
  package->set_name("com.example.package");

  std::string serialized = trace.SerializeAsString();

  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor::Config config;
  config.verify = false;

  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = "com.example.package";

  auto status =
      redactor->Redact(input_file.path(), output_file.path(), &context);

  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(),
            "TraceRedactor: No process timeline found. Are sched_free or "
            "process stats data sources missing");
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_SinglePassAppendsAugmentAtEnd \
  DISABLED_SinglePassAppendsAugmentAtEnd
#else
#define MAYBE_SinglePassAppendsAugmentAtEnd SinglePassAppendsAugmentAtEnd
#endif
TEST(TraceRedactorTest, MAYBE_SinglePassAppendsAugmentAtEnd) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  protos::gen::Trace trace;
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(100);
    packet->set_trusted_uid(1000);
  }
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(200);
    packet->set_trusted_uid(2000);  // Will be dropped by transform
  }
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(300);
    packet->set_trusted_uid(1000);
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor redactor;
  redactor.emplace_collect<DummyCollect>();
  redactor.emplace_transform<DummyTransform>();
  redactor.emplace_augment<DummyAugment>();

  Context context;
  context.package_uid = 1000;
  ASSERT_OK(redactor.Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);
  std::vector<uint64_t> timestamps;
  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());
    timestamps.push_back(p.timestamp());
  }

  // Packets should be: timestamp 100, timestamp 300, and augmented packet 999.
  ASSERT_EQ(timestamps.size(), 3u);
  EXPECT_EQ(timestamps[0], 100u);
  EXPECT_EQ(timestamps[1], 300u);
  EXPECT_EQ(timestamps[2], 999u);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_MultiPassPipelineExecution DISABLED_MultiPassPipelineExecution
#else
#define MAYBE_MultiPassPipelineExecution MultiPassPipelineExecution
#endif
TEST(TraceRedactorTest, MAYBE_MultiPassPipelineExecution) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  protos::gen::Trace trace;
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(10);
    packet->set_trusted_uid(500);
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor redactor;
  auto* pass1 = redactor.add_pass();
  pass1->emplace_augment<DummyAugment>();

  // Pass 2 also augments a packet
  auto* pass2 = redactor.add_pass();
  pass2->emplace_augment<DummyAugment>();

  Context context;
  context.package_uid = 500;
  ASSERT_OK(redactor.Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);
  std::vector<uint64_t> timestamps;
  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());
    timestamps.push_back(p.timestamp());
  }

  // Initial packet (timestamp 10), Pass 1 augmented packet (timestamp 999),
  // Pass 2 augmented packet (timestamp 999)
  ASSERT_EQ(timestamps.size(), 3u);
  EXPECT_EQ(timestamps[0], 10u);
  EXPECT_EQ(timestamps[1], 999u);
  EXPECT_EQ(timestamps[2], 999u);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_ThreePassPipelineExecution DISABLED_ThreePassPipelineExecution
#else
#define MAYBE_ThreePassPipelineExecution ThreePassPipelineExecution
#endif
TEST(TraceRedactorTest, MAYBE_ThreePassPipelineExecution) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  protos::gen::Trace trace;
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(10);
    packet->set_trusted_uid(500);
  }
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(20);
    packet->set_trusted_uid(999);  // Will be dropped by pass 2 transform
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor redactor;

  // Pass 1: Collect package_uid and augment a packet (ts = 100)
  auto* pass1 = redactor.add_pass();
  pass1->emplace_collect<DummyCollect>();
  pass1->emplace_augment<DummyAugmentTimestamp<100>>();

  // Pass 2: Transform (drop untrusted uid 999) and augment a packet (ts = 200)
  auto* pass2 = redactor.add_pass();
  pass2->emplace_transform<DummyTransform>();
  pass2->emplace_augment<DummyAugmentTimestamp<200>>();

  // Pass 3: Augment a packet (ts = 300) - exercises buffer recycling back to
  // buffer_a
  auto* pass3 = redactor.add_pass();
  pass3->emplace_augment<DummyAugmentTimestamp<300>>();

  Context context;
  ASSERT_OK(redactor.Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);
  std::vector<uint64_t> timestamps;
  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());
    timestamps.push_back(p.timestamp());
  }

  // Expect:
  // 1. Initial packet (ts = 10)
  // 2. Pass 1 augment (ts = 100)
  // 3. Pass 2 augment (ts = 200)
  // 4. Pass 3 augment (ts = 300)
  // (Initial packet with ts = 20 was dropped by pass 2)
  ASSERT_EQ(timestamps.size(), 4u);
  EXPECT_EQ(timestamps[0], 10u);
  EXPECT_EQ(timestamps[1], 100u);
  EXPECT_EQ(timestamps[2], 200u);
  EXPECT_EQ(timestamps[3], 300u);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_RedactTrace_IsolatesMultiUserInstances \
  DISABLED_RedactTrace_IsolatesMultiUserInstances
#else
#define MAYBE_RedactTrace_IsolatesMultiUserInstances \
  RedactTrace_IsolatesMultiUserInstances
#endif
TEST(TraceRedactorTest, MAYBE_RedactTrace_IsolatesMultiUserInstances) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  constexpr uint64_t kUser0Uid = 10234;
  constexpr uint64_t kUser10Uid = 1010234;
  constexpr int32_t kUser0Pid = 100;
  constexpr int32_t kUser10Pid = 200;

  protos::gen::Trace trace;

  // Packet 1: PackagesList containing the package with base UID (10234)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(10);
    packet->set_trusted_uid(9999);
    auto* packages = packet->mutable_packages_list();
    auto* pkg = packages->add_packages();
    pkg->set_name("com.example.app");
    pkg->set_uid(kUser0Uid);
  }

  // Packet 2: ProcessTree containing Process A (User 0) and Process B (User 10)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(20);
    auto* pt = packet->mutable_process_tree();

    auto* pA = pt->add_processes();
    pA->set_pid(kUser0Pid);
    pA->set_ppid(1);
    pA->set_uid(static_cast<uint32_t>(kUser0Uid));

    auto* pB = pt->add_processes();
    pB->set_pid(kUser10Pid);
    pB->set_ppid(1);
    pB->set_uid(static_cast<uint32_t>(kUser10Uid));
  }

  // Packet 3: FtraceEvents with task_rename and sched_switch for both processes
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(30);
    auto* bundle = packet->mutable_ftrace_events();
    bundle->set_cpu(0);

    // Event 1: User 0 task_rename
    auto* e1 = bundle->add_event();
    e1->set_timestamp(30);
    e1->set_pid(kUser0Pid);
    auto* rename0 = e1->mutable_task_rename();
    rename0->set_pid(kUser0Pid);
    rename0->set_newcomm("User0App");
    rename0->set_oldcomm("User0App");
    rename0->set_oom_score_adj(0);

    // Event 2: User 0 sched_switch
    auto* e2 = bundle->add_event();
    e2->set_timestamp(31);
    e2->set_pid(kUser0Pid);
    auto* switch0 = e2->mutable_sched_switch();
    switch0->set_prev_pid(kUser0Pid);
    switch0->set_prev_comm("User0App");
    switch0->set_prev_prio(120);
    switch0->set_prev_state(0);
    switch0->set_next_pid(1);
    switch0->set_next_comm("init");
    switch0->set_next_prio(120);

    // Event 3: User 10 task_rename
    auto* e3 = bundle->add_event();
    e3->set_timestamp(32);
    e3->set_pid(kUser10Pid);
    auto* rename10 = e3->mutable_task_rename();
    rename10->set_pid(kUser10Pid);
    rename10->set_newcomm("User10App");
    rename10->set_oldcomm("User10App");
    rename10->set_oom_score_adj(0);

    // Event 4: User 10 sched_switch
    auto* e4 = bundle->add_event();
    e4->set_timestamp(33);
    e4->set_pid(kUser10Pid);
    auto* switch10 = e4->mutable_sched_switch();
    switch10->set_prev_pid(kUser10Pid);
    switch10->set_prev_comm("User10App");
    switch10->set_prev_prio(120);
    switch10->set_prev_state(0);
    switch10->set_next_pid(1);
    switch10->set_next_comm("init");
    switch10->set_next_prio(120);
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor::Config config;
  config.verify = false;
  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = "com.example.app";
  // Target User 10 explicitly
  context.package_uid = kUser10Uid;

  ASSERT_OK(redactor->Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);

  bool found_packages_list = false;
  bool found_user10_rename = false;
  bool found_user10_switch = false;
  bool found_user0_comm = false;

  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());

    if (p.has_packages_list()) {
      protos::pbzero::PackagesList::Decoder pl(p.packages_list());
      for (auto pkg_it = pl.packages(); pkg_it; ++pkg_it) {
        protos::pbzero::PackagesList::PackageInfo::Decoder pkg(
            pkg_it->as_bytes());
        if (pkg.name().ToStdString() == "com.example.app") {
          found_packages_list = true;
        }
      }
    }

    if (p.has_ftrace_events()) {
      protos::pbzero::FtraceEventBundle::Decoder bundle(p.ftrace_events());
      for (auto e_it = bundle.event(); e_it; ++e_it) {
        protos::pbzero::FtraceEvent::Decoder e(e_it->as_bytes());

        if (e.has_task_rename()) {
          protos::pbzero::TaskRenameFtraceEvent::Decoder tr(e.task_rename());
          if (tr.newcomm().ToStdString() == "User10App") {
            found_user10_rename = true;
          }
          if (tr.newcomm().ToStdString() == "User0App") {
            found_user0_comm = true;
          }
        }

        if (e.has_sched_switch()) {
          protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(e.sched_switch());
          if (ss.prev_comm().ToStdString() == "User10App") {
            found_user10_switch = true;
          }
          if (ss.prev_comm().ToStdString() == "User0App") {
            found_user0_comm = true;
          }
        }
      }
    }
  }

  // Verify: PackagesList entry was preserved via ToAppId
  EXPECT_TRUE(found_packages_list);

  // Verify: User 10 (target) events survive intact
  EXPECT_TRUE(found_user10_rename);
  EXPECT_TRUE(found_user10_switch);

  // Verify: User 0 comm was scrubbed / redacted
  EXPECT_FALSE(found_user0_comm);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_RedactTrace_IsolatesMultiUserInstances_User0Target \
  DISABLED_RedactTrace_IsolatesMultiUserInstances_User0Target
#else
#define MAYBE_RedactTrace_IsolatesMultiUserInstances_User0Target \
  RedactTrace_IsolatesMultiUserInstances_User0Target
#endif
TEST(TraceRedactorTest,
     MAYBE_RedactTrace_IsolatesMultiUserInstances_User0Target) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  constexpr uint64_t kUser0Uid = 10234;
  constexpr uint64_t kUser10Uid = 1010234;
  constexpr int32_t kUser0Pid = 100;
  constexpr int32_t kUser10Pid = 200;

  protos::gen::Trace trace;

  // Packet 1: PackagesList containing the package with base UID (10234)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(10);
    packet->set_trusted_uid(9999);
    auto* packages = packet->mutable_packages_list();
    auto* pkg = packages->add_packages();
    pkg->set_name("com.example.app");
    pkg->set_uid(kUser0Uid);
  }

  // Packet 2: ProcessTree containing Process A (User 0) and Process B (User 10)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(20);
    auto* pt = packet->mutable_process_tree();

    auto* pA = pt->add_processes();
    pA->set_pid(kUser0Pid);
    pA->set_ppid(1);
    pA->set_uid(static_cast<uint32_t>(kUser0Uid));

    auto* pB = pt->add_processes();
    pB->set_pid(kUser10Pid);
    pB->set_ppid(1);
    pB->set_uid(static_cast<uint32_t>(kUser10Uid));
  }

  // Packet 3: FtraceEvents with task_rename and sched_switch for both processes
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(30);
    auto* bundle = packet->mutable_ftrace_events();
    bundle->set_cpu(0);

    // Event 1: User 0 task_rename
    auto* e1 = bundle->add_event();
    e1->set_timestamp(30);
    e1->set_pid(kUser0Pid);
    auto* rename0 = e1->mutable_task_rename();
    rename0->set_pid(kUser0Pid);
    rename0->set_newcomm("User0App");
    rename0->set_oldcomm("User0App");
    rename0->set_oom_score_adj(0);

    // Event 2: User 0 sched_switch
    auto* e2 = bundle->add_event();
    e2->set_timestamp(31);
    e2->set_pid(kUser0Pid);
    auto* switch0 = e2->mutable_sched_switch();
    switch0->set_prev_pid(kUser0Pid);
    switch0->set_prev_comm("User0App");
    switch0->set_prev_prio(120);
    switch0->set_prev_state(0);
    switch0->set_next_pid(1);
    switch0->set_next_comm("init");
    switch0->set_next_prio(120);

    // Event 3: User 10 task_rename
    auto* e3 = bundle->add_event();
    e3->set_timestamp(32);
    e3->set_pid(kUser10Pid);
    auto* rename10 = e3->mutable_task_rename();
    rename10->set_pid(kUser10Pid);
    rename10->set_newcomm("User10App");
    rename10->set_oldcomm("User10App");
    rename10->set_oom_score_adj(0);

    // Event 4: User 10 sched_switch
    auto* e4 = bundle->add_event();
    e4->set_timestamp(33);
    e4->set_pid(kUser10Pid);
    auto* switch10 = e4->mutable_sched_switch();
    switch10->set_prev_pid(kUser10Pid);
    switch10->set_prev_comm("User10App");
    switch10->set_prev_prio(120);
    switch10->set_prev_state(0);
    switch10->set_next_pid(1);
    switch10->set_next_comm("init");
    switch10->set_next_prio(120);
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor::Config config;
  config.verify = false;
  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = "com.example.app";
  // Target User 0 explicitly
  context.package_uid = kUser0Uid;

  ASSERT_OK(redactor->Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);

  bool found_packages_list = false;
  bool found_user0_rename = false;
  bool found_user0_switch = false;
  bool found_user10_comm = false;

  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());

    if (p.has_packages_list()) {
      protos::pbzero::PackagesList::Decoder pl(p.packages_list());
      for (auto pkg_it = pl.packages(); pkg_it; ++pkg_it) {
        protos::pbzero::PackagesList::PackageInfo::Decoder pkg(
            pkg_it->as_bytes());
        if (pkg.name().ToStdString() == "com.example.app") {
          found_packages_list = true;
        }
      }
    }

    if (p.has_ftrace_events()) {
      protos::pbzero::FtraceEventBundle::Decoder bundle(p.ftrace_events());
      for (auto e_it = bundle.event(); e_it; ++e_it) {
        protos::pbzero::FtraceEvent::Decoder e(e_it->as_bytes());

        if (e.has_task_rename()) {
          protos::pbzero::TaskRenameFtraceEvent::Decoder tr(e.task_rename());
          if (tr.newcomm().ToStdString() == "User0App") {
            found_user0_rename = true;
          }
          if (tr.newcomm().ToStdString() == "User10App") {
            found_user10_comm = true;
          }
        }

        if (e.has_sched_switch()) {
          protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(e.sched_switch());
          if (ss.prev_comm().ToStdString() == "User0App") {
            found_user0_switch = true;
          }
          if (ss.prev_comm().ToStdString() == "User10App") {
            found_user10_comm = true;
          }
        }
      }
    }
  }

  // Verify: PackagesList entry was preserved via ToAppId
  EXPECT_TRUE(found_packages_list);

  // Verify: User 0 (target) events survive intact
  EXPECT_TRUE(found_user0_rename);
  EXPECT_TRUE(found_user0_switch);

  // Verify: User 10 comm was scrubbed / redacted
  EXPECT_FALSE(found_user10_comm);
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define MAYBE_RedactTrace_SecondaryUserAboveTen \
  DISABLED_RedactTrace_SecondaryUserAboveTen
#else
#define MAYBE_RedactTrace_SecondaryUserAboveTen \
  RedactTrace_SecondaryUserAboveTen
#endif
TEST(TraceRedactorTest, MAYBE_RedactTrace_SecondaryUserAboveTen) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  constexpr uint64_t kUser0Uid = 10234;
  constexpr uint64_t kUser11Uid = 1110234;
  constexpr uint64_t kUser12Uid = 1210234;
  constexpr int32_t kUser11Pid = 300;
  constexpr int32_t kUser12Pid = 400;

  protos::gen::Trace trace;

  // Packet 1: PackagesList containing the package with base UID (10234)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(10);
    packet->set_trusted_uid(9999);
    auto* packages = packet->mutable_packages_list();
    auto* pkg = packages->add_packages();
    pkg->set_name("com.example.app");
    pkg->set_uid(kUser0Uid);
  }

  // Packet 2: ProcessTree containing Process C (User 11) and Process D (User
  // 12)
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(20);
    auto* pt = packet->mutable_process_tree();

    auto* pC = pt->add_processes();
    pC->set_pid(kUser11Pid);
    pC->set_ppid(1);
    pC->set_uid(static_cast<uint32_t>(kUser11Uid));

    auto* pD = pt->add_processes();
    pD->set_pid(kUser12Pid);
    pD->set_ppid(1);
    pD->set_uid(static_cast<uint32_t>(kUser12Uid));
  }

  // Packet 3: FtraceEvents with task_rename and sched_switch
  {
    auto* packet = trace.add_packet();
    packet->set_timestamp(30);
    auto* bundle = packet->mutable_ftrace_events();
    bundle->set_cpu(0);

    // Event 1: User 11 task_rename
    auto* e1 = bundle->add_event();
    e1->set_timestamp(30);
    e1->set_pid(kUser11Pid);
    auto* rename11 = e1->mutable_task_rename();
    rename11->set_pid(kUser11Pid);
    rename11->set_newcomm("User11App");
    rename11->set_oldcomm("User11App");
    rename11->set_oom_score_adj(0);

    // Event 2: User 11 sched_switch
    auto* e2 = bundle->add_event();
    e2->set_timestamp(31);
    e2->set_pid(kUser11Pid);
    auto* switch11 = e2->mutable_sched_switch();
    switch11->set_prev_pid(kUser11Pid);
    switch11->set_prev_comm("User11App");
    switch11->set_prev_prio(120);
    switch11->set_prev_state(0);
    switch11->set_next_pid(1);
    switch11->set_next_comm("init");
    switch11->set_next_prio(120);

    // Event 3: User 12 task_rename
    auto* e3 = bundle->add_event();
    e3->set_timestamp(32);
    e3->set_pid(kUser12Pid);
    auto* rename12 = e3->mutable_task_rename();
    rename12->set_pid(kUser12Pid);
    rename12->set_newcomm("User12App");
    rename12->set_oldcomm("User12App");
    rename12->set_oom_score_adj(0);

    // Event 4: User 12 sched_switch
    auto* e4 = bundle->add_event();
    e4->set_timestamp(33);
    e4->set_pid(kUser12Pid);
    auto* switch12 = e4->mutable_sched_switch();
    switch12->set_prev_pid(kUser12Pid);
    switch12->set_prev_comm("User12App");
    switch12->set_prev_prio(120);
    switch12->set_prev_state(0);
    switch12->set_next_pid(1);
    switch12->set_next_comm("init");
    switch12->set_next_prio(120);
  }

  std::string serialized = trace.SerializeAsString();
  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor::Config config;
  config.verify = false;
  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = "com.example.app";
  // Target User 11 explicitly
  context.package_uid = kUser11Uid;

  ASSERT_OK(redactor->Redact(input_file.path(), output_file.path(), &context));

  std::string output_content;
  ASSERT_TRUE(base::ReadFile(output_file.path(), &output_content));

  protos::pbzero::Trace::Decoder output_trace(output_content);

  bool found_packages_list = false;
  bool found_user11_rename = false;
  bool found_user11_switch = false;
  bool found_user12_comm = false;

  for (auto it = output_trace.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder p(it->as_bytes());

    if (p.has_packages_list()) {
      protos::pbzero::PackagesList::Decoder pl(p.packages_list());
      for (auto pkg_it = pl.packages(); pkg_it; ++pkg_it) {
        protos::pbzero::PackagesList::PackageInfo::Decoder pkg(
            pkg_it->as_bytes());
        if (pkg.name().ToStdString() == "com.example.app") {
          found_packages_list = true;
        }
      }
    }

    if (p.has_ftrace_events()) {
      protos::pbzero::FtraceEventBundle::Decoder bundle(p.ftrace_events());
      for (auto e_it = bundle.event(); e_it; ++e_it) {
        protos::pbzero::FtraceEvent::Decoder e(e_it->as_bytes());

        if (e.has_task_rename()) {
          protos::pbzero::TaskRenameFtraceEvent::Decoder tr(e.task_rename());
          if (tr.newcomm().ToStdString() == "User11App") {
            found_user11_rename = true;
          }
          if (tr.newcomm().ToStdString() == "User12App") {
            found_user12_comm = true;
          }
        }

        if (e.has_sched_switch()) {
          protos::pbzero::SchedSwitchFtraceEvent::Decoder ss(e.sched_switch());
          if (ss.prev_comm().ToStdString() == "User11App") {
            found_user11_switch = true;
          }
          if (ss.prev_comm().ToStdString() == "User12App") {
            found_user12_comm = true;
          }
        }
      }
    }
  }

  // Verify: PackagesList entry was preserved via ToAppId
  EXPECT_TRUE(found_packages_list);

  // Verify: User 11 (target) events survive intact
  EXPECT_TRUE(found_user11_rename);
  EXPECT_TRUE(found_user11_switch);

  // Verify: User 12 comm was scrubbed / redacted
  EXPECT_FALSE(found_user12_comm);
}

}  // namespace perfetto::trace_redaction
