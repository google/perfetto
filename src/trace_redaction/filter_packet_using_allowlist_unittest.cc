/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

// TODO(vaage): These tests were used to test the filter-driver, but these tests
// no longer do that. A new test suite should be created to test the driver code
// with the different filters.
namespace perfetto::trace_redaction {

namespace {

constexpr auto kJustSomeFieldId =
    protos::pbzero::TracePacket::kProcessTreeFieldNumber;

}  // namespace

TEST(FilterPacketUsingAllowlistParamErrorTest, ReturnsErrorForEmptyAllowlist) {
  Context context;

  FilterPacketUsingAllowlist filter;
  auto status = filter.VerifyContext(context);

  ASSERT_FALSE(status.ok()) << status.message();
}

TEST(FilterPacketUsingAllowlistParamErrorTest, ReturnsFalseForInvalidField) {
  // Have something in the allow-list to avoid an error.
  Context context;
  context.trace_packet_allow_list.insert(kJustSomeFieldId);

  protozero::Field invalid = {};
  ASSERT_FALSE(invalid.valid());

  FilterPacketUsingAllowlist filter;
  ASSERT_FALSE(filter.KeepField(context, invalid));
}

TEST(FilterPacketUsingAllowlistParamErrorTest, ReturnsFalseForExcludedField) {
  Context context;
  context.trace_packet_allow_list.insert(kJustSomeFieldId);

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  packet->set_timestamp(123456789);

  auto buffer = packet.SerializeAsString();

  protozero::ProtoDecoder decoder(buffer);
  protozero::Field field = decoder.FindField(kJustSomeFieldId);

  FilterPacketUsingAllowlist filter;
  ASSERT_FALSE(filter.KeepField(context, field));
}

}  // namespace perfetto::trace_redaction
