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

#include <cstdint>
#include <optional>
#include <vector>

#include "test/gtest_and_gmock.h"

#include "src/base/test/test_task_runner.h"
#include "src/trace_processor/util/decompressor.h"
#include "test/test_helper.h"

#include "protos/perfetto/common/descriptor.gen.h"
#include "protos/perfetto/trace/extension_descriptor.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

// CTS coverage for the out-of-tree proto extension descriptors traced loads on
// Android from /etc/tracing_descriptors.gz and
// /vendor/etc/tracing_descriptors.gz (RFC-0017). traced emits their gzipped
// contents into every trace as TracePacket.extension_descriptor packets tagged
// with the source path.
//
// Core trace payloads (heap graph, android track events, ProtoLog, Winscope,
// ...) are defined out-of-tree, so the system descriptor is mandatory and must
// describe those relocated fields.

namespace perfetto {
namespace {

// Paths as recorded in ExtensionDescriptor.file_name. traced opens "/etc" (a
// symlink to /system/etc), so the system descriptor uses the "/etc" prefix.
constexpr char kSystemDescriptorPath[] = "/etc/tracing_descriptors.gz";
constexpr char kVendorDescriptorPath[] = "/vendor/etc/tracing_descriptors.gz";

constexpr char kTracePacketExtendee[] = ".perfetto.protos.TracePacket";
constexpr char kTrackEventExtendee[] = ".perfetto.protos.TrackEvent";
constexpr char kWinscopeExtensionsExtendee[] =
    ".com.android.internal.WinscopeExtensions";

// Payloads the system descriptor must describe. The numbers are wire-locked
// extension tags: the TracePacket ones are documented in
// protos/perfetto/trace/trace_packet.proto, the rest are allocated in the
// Android tree.
constexpr int32_t kHeapGraphField = 56;            // TracePacket (ART)
constexpr int32_t kProtoLogMessageField = 104;     // TracePacket (ProtoLog)
constexpr int32_t kAppWakelockBundleField = 116;   // TracePacket (wakelocks)
constexpr int32_t kProcessStartEventField = 2010;  // TrackEvent (android)
constexpr int32_t kWindowManagerField = 6;         // WinscopeExtensions (WM)

std::vector<protos::gen::TracePacket> CollectTrace() {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  // Descriptors are emitted as initial packets, independent of any data source.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096);
  trace_config.set_duration_ms(1);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();
  // extension_descriptor packets are filtered out of trace(); use full_trace().
  return helper.full_trace();
}

// Returns the descriptor emitted for `path`, or nullptr if absent.
const protos::gen::ExtensionDescriptor* FindDescriptor(
    const std::vector<protos::gen::TracePacket>& packets,
    const std::string& path) {
  for (const auto& packet : packets) {
    if (packet.has_extension_descriptor() &&
        packet.extension_descriptor().file_name() == path) {
      return &packet.extension_descriptor();
    }
  }
  return nullptr;
}

// Decompresses and parses `ext` into `fds`, mirroring how ProtoTraceReader
// consumes it.
void ParseDescriptor(const protos::gen::ExtensionDescriptor& ext,
                     protos::gen::FileDescriptorSet* fds) {
  // Android descriptors are always gzipped.
  ASSERT_TRUE(ext.has_extension_set_gzip());
  ASSERT_FALSE(ext.has_extension_set());
  const std::string& gz = ext.extension_set_gzip();
  ASSERT_GE(gz.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(gz[0]), 0x1f);  // gzip magic
  EXPECT_EQ(static_cast<uint8_t>(gz[1]), 0x8b);

  std::optional<trace_processor::util::DecompressedBuffer> raw =
      trace_processor::util::DecompressToBuffer(
          trace_processor::util::CompressionType::kGzip,
          reinterpret_cast<const uint8_t*>(gz.data()), gz.size());
  ASSERT_TRUE(raw);
  ASSERT_GT(raw->size, 0u);
  ASSERT_TRUE(
      fds->ParseFromArray(raw->data.get(), static_cast<int>(raw->size)));
  EXPECT_GT(fds->file_size(), 0);
}

// True if `msg` or a nested message declares an extension of `extendee` with
// `number`. Payloads live inside a wrapper message (e.g.
// ArtHeapGraphTracePacket), not at file scope, so the search recurses.
bool MessageHasExtension(const protos::gen::DescriptorProto& msg,
                         const std::string& extendee,
                         int32_t number) {
  for (const auto& ext : msg.extension()) {
    if (ext.number() == number && ext.extendee() == extendee)
      return true;
  }
  for (const auto& nested : msg.nested_type()) {
    if (MessageHasExtension(nested, extendee, number))
      return true;
  }
  return false;
}

// True if any file declares an extension of `extendee` with `number`, whether
// top-level or nested inside a message.
bool HasExtension(const protos::gen::FileDescriptorSet& fds,
                  const std::string& extendee,
                  int32_t number) {
  for (const auto& file : fds.file()) {
    for (const auto& ext : file.extension()) {
      if (ext.number() == number && ext.extendee() == extendee)
        return true;
    }
    for (const auto& msg : file.message_type()) {
      if (MessageHasExtension(msg, extendee, number))
        return true;
    }
  }
  return false;
}

// The system descriptor is mandatory and must describe the relocated payloads.
TEST(ExtensionDescriptorCtsTest, TestSystemExtensionDescriptor) {
  auto packets = CollectTrace();
  const auto* ext = FindDescriptor(packets, kSystemDescriptorPath);
  ASSERT_NE(ext, nullptr) << kSystemDescriptorPath
                          << " missing: the system image must ship the "
                             "out-of-tree tracing descriptors (RFC-0017).";

  protos::gen::FileDescriptorSet fds;
  ASSERT_NO_FATAL_FAILURE(ParseDescriptor(*ext, &fds));

  // The relocated core payloads must all be present, covering the TracePacket,
  // TrackEvent and WinscopeExtensions scopes.
  EXPECT_TRUE(HasExtension(fds, kTracePacketExtendee, kHeapGraphField));
  EXPECT_TRUE(HasExtension(fds, kTracePacketExtendee, kProtoLogMessageField));
  EXPECT_TRUE(HasExtension(fds, kTracePacketExtendee, kAppWakelockBundleField));
  EXPECT_TRUE(HasExtension(fds, kTrackEventExtendee, kProcessStartEventField));
  EXPECT_TRUE(
      HasExtension(fds, kWinscopeExtensionsExtendee, kWindowManagerField));
}

// Every emitted descriptor must come from a known path and be well-formed. A
// vendor descriptor is not guaranteed on every device, so it is only validated
// when present.
TEST(ExtensionDescriptorCtsTest, TestExtensionDescriptorsWellFormed) {
  auto packets = CollectTrace();
  bool found_system = false;
  for (const auto& packet : packets) {
    if (!packet.has_extension_descriptor())
      continue;
    const auto& ext = packet.extension_descriptor();
    EXPECT_TRUE(ext.file_name() == kSystemDescriptorPath ||
                ext.file_name() == kVendorDescriptorPath)
        << "unexpected descriptor path: " << ext.file_name();

    protos::gen::FileDescriptorSet fds;
    ASSERT_NO_FATAL_FAILURE(ParseDescriptor(ext, &fds));
    found_system |= ext.file_name() == kSystemDescriptorPath;
  }
  EXPECT_TRUE(found_system);
}

}  // namespace
}  // namespace perfetto
