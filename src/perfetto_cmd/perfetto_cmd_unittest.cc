/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/perfetto_cmd/perfetto_cmd.h"

#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/perfetto_cmd/packet_writer.h"

#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {

class PerfettoCmdlineUnitTest : public ::testing::Test {
 protected:
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  static std::optional<TraceConfig> ParseTraceConfigFromMmapedTrace(
      base::ScopedMmap mmapped_trace) {
    return PerfettoCmd::ParseTraceConfigFromMmapedTrace(
        std::move(mmapped_trace));
  }
#endif
};

namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

// Copied from src/perfetto_cmd/packet_writer_unittest.cc
template <typename F>
TracePacket CreateTracePacket(F fill_function) {
  protos::gen::TracePacket msg;
  fill_function(&msg);
  std::vector<uint8_t> buf = msg.SerializeAsArray();
  Slice slice = Slice::Allocate(buf.size());
  memcpy(slice.own_data(), buf.data(), buf.size());
  perfetto::TracePacket packet;
  packet.AddSlice(std::move(slice));
  return packet;
}

static void WritePacketsToFile(const std::vector<TracePacket>& packets,
                               const std::string& path) {
  auto fstream = base::OpenFstream(path, "w");
  PacketWriter pw(*fstream);
  pw.WritePackets(packets);
}

TEST_F(PerfettoCmdlineUnitTest, ParseTraceConfigFromInvalidTrace) {
  {
    // We would like to treat empty files as invalid traces, but mmap-ing
    // an empty file returns an error. We should always check if the file is
    // empty before mmap-ing it, to see the difference between empty file and an
    // actual mmaping error.
    base::TempFile empty_file = base::TempFile::Create();
    base::ScopedMmap mmaped =
        base::ReadMmapWholeFile(empty_file.path().c_str());
    ASSERT_FALSE(mmaped.IsValid());
  }
  {
    base::TempFile text_file = base::TempFile::Create();
    std::string data = "This is a text file!";
    base::WriteAll(text_file.fd(), data.data(), data.size());
    base::ScopedMmap mmaped = base::ReadMmapWholeFile(text_file.path());
    std::optional result = ParseTraceConfigFromMmapedTrace(std::move(mmaped));
    ASSERT_FALSE(result.has_value());
  }
}

TEST_F(PerfettoCmdlineUnitTest, ParseTraceConfigFromTrace) {
  // Trace with a reporter config and correct trusted_uid.
  {
    base::TempFile trace_file = base::TempFile::Create();
    {
      std::vector<perfetto::TracePacket> packets;
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->set_trusted_uid(9999);
        auto config = msg->mutable_trace_config();
        config->set_trace_uuid_lsb(555);
        config->set_trace_uuid_msb(888);
        config->set_unique_session_name("my_name");
        config->mutable_android_report_config()->set_reporter_service_class(
            "reporter");
      }));
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->mutable_for_testing()->set_str("payload");
      }));

      WritePacketsToFile(packets, trace_file.path());
    }

    base::ScopedMmap mmaped = base::ReadMmapWholeFile(trace_file.path());
    std::optional result = ParseTraceConfigFromMmapedTrace(std::move(mmaped));
    EXPECT_EQ(result->trace_uuid_lsb(), 555);
    EXPECT_EQ(result->trace_uuid_msb(), 888);
    EXPECT_EQ(result->unique_session_name(), "my_name");
    EXPECT_EQ(result->android_report_config().reporter_service_class(),
              "reporter");
  }

  // Trace without an config.
  {
    base::TempFile trace_file = base::TempFile::Create();
    {
      std::vector<perfetto::TracePacket> packets;
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->set_trusted_uid(9999);
        msg->mutable_for_testing()->set_str("payload#1");
      }));
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->mutable_for_testing()->set_str("payload#2");
      }));

      WritePacketsToFile(packets, trace_file.path());
    }

    base::ScopedMmap mmaped = base::ReadMmapWholeFile(trace_file.path());
    std::optional result = ParseTraceConfigFromMmapedTrace(std::move(mmaped));
    EXPECT_FALSE(result.has_value());
  }

  // Trace with a potentially harmful android reporter config without
  // trusted_uid.
  {
    base::TempFile trace_file = base::TempFile::Create();
    {
      std::vector<perfetto::TracePacket> packets;
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        auto config = msg->mutable_trace_config();
        config->set_unique_session_name("my_session");
        config->mutable_android_report_config()->set_reporter_service_class(
            "bad_reporter");
      }));
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->mutable_for_testing()->set_str("payload");
      }));
      packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
        msg->set_trusted_uid(9999);
        auto android_config =
            msg->mutable_trace_config()->mutable_android_report_config();
        android_config->set_reporter_service_class("good_reporter");
        android_config->set_use_pipe_in_framework_for_testing(true);
      }));

      WritePacketsToFile(packets, trace_file.path());
    }

    base::ScopedMmap mmaped = base::ReadMmapWholeFile(trace_file.path());
    std::optional result = ParseTraceConfigFromMmapedTrace(std::move(mmaped));
    EXPECT_FALSE(result->has_unique_session_name());
    EXPECT_EQ(result->android_report_config().reporter_service_class(),
              "good_reporter");
    EXPECT_EQ(
        result->android_report_config().use_pipe_in_framework_for_testing(),
        true);
  }
}
#endif

}  // namespace
}  // namespace perfetto
