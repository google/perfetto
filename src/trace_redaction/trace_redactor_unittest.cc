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
#include "protos/perfetto/trace/ps/process_tree.gen.h"
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

}  // namespace perfetto::trace_redaction
