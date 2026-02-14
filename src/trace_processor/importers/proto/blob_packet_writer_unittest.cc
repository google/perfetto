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

#include "src/trace_processor/importers/proto/blob_packet_writer.h"

#include <cstdint>

#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(BlobPacketWriterTest, SinglePacketRoundTrip) {
  BlobPacketWriter writer;

  TraceBlobView tbv =
      writer.WritePacket([](auto* pkt) { pkt->set_timestamp(42); });
  ASSERT_GT(tbv.length(), 0u);

  protos::pbzero::TracePacket::Decoder decoded(tbv.data(), tbv.length());
  EXPECT_TRUE(decoded.has_timestamp());
  EXPECT_EQ(decoded.timestamp(), 42u);
}

TEST(BlobPacketWriterTest, MultiplePacketsShareBlob) {
  BlobPacketWriter writer;

  // Write two packets and verify they share the same underlying blob.
  TraceBlobView tbv1 =
      writer.WritePacket([](auto* pkt) { pkt->set_timestamp(100); });

  TraceBlobView tbv2 =
      writer.WritePacket([](auto* pkt) { pkt->set_timestamp(200); });

  // Both should decode correctly.
  protos::pbzero::TracePacket::Decoder d1(tbv1.data(), tbv1.length());
  EXPECT_EQ(d1.timestamp(), 100u);

  protos::pbzero::TracePacket::Decoder d2(tbv2.data(), tbv2.length());
  EXPECT_EQ(d2.timestamp(), 200u);

  // Both views should point into the same underlying blob (shared via RefPtr).
  // tbv2's data should start after tbv1's data.
  EXPECT_EQ(tbv2.data(), tbv1.data() + tbv1.length());
}

TEST(BlobPacketWriterTest, NestedMessage) {
  BlobPacketWriter writer;

  TraceBlobView tbv = writer.WritePacket([](auto* pkt) {
    pkt->set_timestamp(99);
    // Use set_trusted_uid as a simple nested field that doesn't require
    // additional includes.
    pkt->set_trusted_uid(42);
  });

  protos::pbzero::TracePacket::Decoder decoded(tbv.data(), tbv.length());
  EXPECT_EQ(decoded.timestamp(), 99u);
  EXPECT_EQ(decoded.trusted_uid(), 42);
}

TEST(BlobPacketWriterTest, BeginEndApi) {
  BlobPacketWriter writer;

  auto* pkt = writer.BeginPacket();
  pkt->set_timestamp(77);
  pkt->set_trusted_uid(3);
  TraceBlobView tbv = writer.EndPacket();

  protos::pbzero::TracePacket::Decoder decoded(tbv.data(), tbv.length());
  EXPECT_EQ(decoded.timestamp(), 77u);
  EXPECT_EQ(decoded.trusted_uid(), 3);
}

}  // namespace
}  // namespace perfetto::trace_processor
