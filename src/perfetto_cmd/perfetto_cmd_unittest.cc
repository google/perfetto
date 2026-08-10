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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

#include "protos/perfetto/common/trace_attributes.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

class PerfettoCmdlineUnitTest : public ::testing::Test {
 protected:
  static std::optional<int> ParseCmdline(PerfettoCmd* cmd,
                                         std::vector<std::string> args) {
    // getopt() expects a null-terminated argv (argv[argc] == nullptr).
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);

    std::optional<int> res = cmd->ParseCmdlineAndMaybeDaemonize(
        static_cast<int>(argv.size()) - 1, argv.data());
    return res;
  }

  static const TraceConfig* GetTraceConfig(const PerfettoCmd& cmd) {
    return cmd.trace_config_.get();
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  static std::optional<TraceConfig> ParseTraceConfigFromMmapedTrace(
      const base::ScopedMmap& mmapped_trace) {
    return PerfettoCmd::ParseTraceConfigFromMmapedTrace(mmapped_trace);
  }
#endif
};

namespace {

TEST_F(PerfettoCmdlineUnitTest, AddAttributeParsesAndStoresAttributes) {
  base::TempFile out_file = base::TempFile::Create();
  PerfettoCmd cmd;

  std::optional<int> res = ParseCmdline(
      &cmd,
      {"perfetto", "--out", out_file.path(), "--time", "1s", "--add-attribute",
       "a=b", "--add-attribute", "k=foo=bar", "--add-attribute",
       "empty=", "--add-attribute", "empty2=", "--add-attribute", "novalue"});
  EXPECT_FALSE(res.has_value());

  const TraceConfig* cfg = GetTraceConfig(cmd);
  ASSERT_NE(cfg, nullptr);
  const auto& attrs = cfg->trace_attributes().attribute();
  ASSERT_EQ(attrs.size(), 5u);
  EXPECT_EQ(attrs[0].key(), "a");
  EXPECT_EQ(attrs[0].string_value(), "b");
  EXPECT_EQ(attrs[1].key(), "k");
  EXPECT_EQ(attrs[1].string_value(), "foo=bar");
  EXPECT_EQ(attrs[2].key(), "empty");
  EXPECT_EQ(attrs[2].string_value(), "");
  EXPECT_EQ(attrs[3].key(), "empty2");
  EXPECT_EQ(attrs[3].string_value(), "");
  EXPECT_EQ(attrs[4].key(), "novalue");
  EXPECT_EQ(attrs[4].string_value(), "");
}

TEST_F(PerfettoCmdlineUnitTest, AddAttributeAllowsMissingEquals) {
  base::TempFile out_file = base::TempFile::Create();
  PerfettoCmd cmd;
  std::optional<int> res =
      ParseCmdline(&cmd, {"perfetto", "--out", out_file.path(), "--time", "1s",
                          "--add-attribute", "novalue"});
  EXPECT_FALSE(res.has_value());

  const TraceConfig* cfg = GetTraceConfig(cmd);
  ASSERT_NE(cfg, nullptr);
  const auto& attrs = cfg->trace_attributes().attribute();
  ASSERT_EQ(attrs.size(), 1u);
  EXPECT_EQ(attrs[0].key(), "novalue");
  EXPECT_EQ(attrs[0].string_value(), "");
}

TEST_F(PerfettoCmdlineUnitTest, AddAttributeRejectsEmptyKey) {
  base::TempFile out_file = base::TempFile::Create();
  PerfettoCmd cmd;
  std::optional<int> res =
      ParseCmdline(&cmd, {"perfetto", "--out", out_file.path(), "--time", "1s",
                          "--add-attribute", "=value"});
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, 1);
}

TEST_F(PerfettoCmdlineUnitTest, AddAttributeRejectsEmptyArgument) {
  base::TempFile out_file = base::TempFile::Create();
  PerfettoCmd cmd;
  std::optional<int> res =
      ParseCmdline(&cmd, {"perfetto", "--out", out_file.path(), "--time", "1s",
                          "--add-attribute", ""});
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, 1);
}

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

TEST_F(PerfettoCmdlineUnitTest,
       TruncatesIncompleteTrailingPacketAndAppendsAfterRebootEvent) {
  base::TempFile trace_file = base::TempFile::Create();
  {
    std::vector<perfetto::TracePacket> packets;
    packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
      msg->set_trusted_uid(9999);
      auto config = msg->mutable_trace_config();
      config->set_trace_uuid_lsb(555);
      config->set_trace_uuid_msb(888);
    }));
    WritePacketsToFile(packets, trace_file.path());
  }

  auto orig_size = base::GetFileSize(trace_file.path());
  ASSERT_TRUE(orig_size.has_value());
  uint64_t valid_bytes = *orig_size;

  // Append 5 garbage bytes to simulate an incomplete trailing packet
  const char garbage[] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
  lseek(trace_file.fd(), 0, SEEK_END);
  base::WriteAll(trace_file.fd(), garbage, sizeof(garbage));

  uint64_t file_with_garbage_size = valid_bytes + sizeof(garbage);

  base::ScopedMmap mmaped = base::ReadMmapWholeFile(trace_file.path());
  ASSERT_TRUE(mmaped.IsValid());

  // Call the production static helper function
  size_t final_offset = PerfettoCmd::SanitizeAndAnnotatePersistentTrace(
      trace_file.fd(), mmaped, "test.tmp");

  EXPECT_GT(final_offset, valid_bytes);

  // Verify resulting trace file can be re-mmapped and contains
  // AfterRebootTraceEvent
  base::ScopedMmap updated_mmaped = base::ReadMmapWholeFile(trace_file.path());
  ASSERT_TRUE(updated_mmaped.IsValid());

  bool found_after_reboot_evt = false;
  protozero::ProtoDecoder updated_decoder(updated_mmaped.data(),
                                          updated_mmaped.length());
  for (auto p = updated_decoder.ReadField(); p;
       p = updated_decoder.ReadField()) {
    if (p.id() ==
        protos::pbzero::TracePacket::kAfterRebootTraceEventFieldNumber) {
      found_after_reboot_evt = true;
      protos::pbzero::AfterRebootTraceEvent::Decoder evt_decoder(p.data(),
                                                                 p.size());
      EXPECT_EQ(evt_decoder.original_file_size(), file_with_garbage_size);
      EXPECT_EQ(evt_decoder.bytes_truncated(), sizeof(garbage));
    }
  }
  EXPECT_TRUE(found_after_reboot_evt);
}

TEST_F(PerfettoCmdlineUnitTest,
       SanitizeAndAnnotatePersistentTraceCleanFileHasZeroTruncatedBytes) {
  base::TempFile trace_file = base::TempFile::Create();
  {
    std::vector<perfetto::TracePacket> packets;
    packets.push_back(CreateTracePacket([](protos::gen::TracePacket* msg) {
      msg->set_trusted_uid(9999);
      auto config = msg->mutable_trace_config();
      config->set_trace_uuid_lsb(111);
      config->set_trace_uuid_msb(222);
    }));
    WritePacketsToFile(packets, trace_file.path());
  }

  auto orig_size = base::GetFileSize(trace_file.path());
  ASSERT_TRUE(orig_size.has_value());

  base::ScopedMmap mmaped = base::ReadMmapWholeFile(trace_file.path());
  ASSERT_TRUE(mmaped.IsValid());

  size_t final_offset = PerfettoCmd::SanitizeAndAnnotatePersistentTrace(
      trace_file.fd(), mmaped, "clean_test.tmp");

  EXPECT_GT(final_offset, *orig_size);

  base::ScopedMmap updated_mmaped = base::ReadMmapWholeFile(trace_file.path());
  ASSERT_TRUE(updated_mmaped.IsValid());

  bool found_after_reboot_evt = false;
  protozero::ProtoDecoder updated_decoder(updated_mmaped.data(),
                                          updated_mmaped.length());
  for (auto p = updated_decoder.ReadField(); p;
       p = updated_decoder.ReadField()) {
    if (p.id() ==
        protos::pbzero::TracePacket::kAfterRebootTraceEventFieldNumber) {
      found_after_reboot_evt = true;
      protos::pbzero::AfterRebootTraceEvent::Decoder evt_decoder(p.data(),
                                                                 p.size());
      EXPECT_EQ(evt_decoder.original_file_size(), *orig_size);
      EXPECT_EQ(evt_decoder.bytes_truncated(), 0u);
    }
  }
  EXPECT_TRUE(found_after_reboot_evt);
}

TEST_F(PerfettoCmdlineUnitTest,
       WaitForPreviousRebootTraceUploadNonExistentFileReturnsImmediately) {
  // Should return immediately when the .tmp trace file does not exist on disk
  std::string non_existent_path =
      "/data/misc/perfetto-traces/persistent/non_existent_session_9999.tmp";
  EXPECT_FALSE(base::FileExists(non_existent_path));

  auto start = base::GetBootTimeNs();
  PerfettoCmd::WaitForPreviousRebootTraceUpload("non_existent_session_9999",
                                                non_existent_path);
  auto elapsed_ns = (base::GetBootTimeNs() - start).count();

  // Assert execution returns immediately (under 100 milliseconds)
  EXPECT_LT(elapsed_ns, 100 * 1000 * 1000LL);
}

TEST_F(PerfettoCmdlineUnitTest,
       WaitForPreviousRebootTraceUploadFileExistsWithPropertySetCrashes) {
  base::TempFile temp_file = base::TempFile::Create();
  std::string path = temp_file.path();
  temp_file.Unlink();
  base::ScopedFile fd = base::OpenFile(path, O_CREAT | O_RDWR, 0600);
  EXPECT_TRUE(base::FileExists(path));

  // Set property indicating previous upload has started or finished
  __system_property_set("traced.reboot_trace_status", "1:100000000");

  EXPECT_DEATH(PerfettoCmd::WaitForPreviousRebootTraceUpload(
                   "finished_session_test", path),
               "still exists on disk even though property is set");
}
#endif

}  // namespace
}  // namespace perfetto
