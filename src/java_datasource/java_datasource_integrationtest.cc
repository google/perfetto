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

// Integration test for the Java DataSource JNI path.
//
// This test exercises the exact same C ABI call sequence that
// perfetto_datasource_jni.cc uses:
//   1. PerfettoDsImplCreate + register callbacks + PerfettoDsImplRegister
//   2. PerfettoDsImplTraceIterateBegin/Next (instance iteration)
//   3. PerfettoDsTracerImplPacketBegin + PerfettoStreamWriterAppendBytes +
//      PerfettoDsTracerImplPacketEnd (packet writing)
//   4. PerfettoDsImplGetIncrementalState (incremental state check)
//
// The packet bytes are pre-encoded in the test (mimicking ProtoWriter output)
// and written to the stream writer via AppendBytes, exactly as the JNI does.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/stream_writer.h"

#include "src/shared_lib/reset_for_testing.h"
#include "src/shared_lib/test/utils.h"

#include "test/gtest_and_gmock.h"

using ::perfetto::shlib::test_utils::FieldView;
using ::perfetto::shlib::test_utils::IdFieldView;
using ::perfetto::shlib::test_utils::MsgField;
using ::perfetto::shlib::test_utils::PbField;
using ::perfetto::shlib::test_utils::StringField;
using ::perfetto::shlib::test_utils::TracingSession;
using ::perfetto::shlib::test_utils::VarIntField;
using ::testing::_;
using ::testing::ElementsAre;

namespace {

constexpr char kDataSourceName[] = "dev.perfetto.java_datasource_test";

// Per-instance incremental state, same as JNI IncrState.
struct IncrState {
  bool was_cleared;
};

void* OnCreateIncr(struct PerfettoDsImpl*,
                   PerfettoDsInstanceIndex,
                   struct PerfettoDsTracerImpl*,
                   void*) {
  auto* incr = new IncrState();
  incr->was_cleared = true;
  return incr;
}

void OnDeleteIncr(void* obj) {
  delete static_cast<IncrState*>(obj);
}

bool OnClearIncr(void* obj, void*) {
  auto* incr = static_cast<IncrState*>(obj);
  incr->was_cleared = true;
  return true;
}

// Helper: encode a varint into buf. Returns number of bytes written.
size_t EncodeVarInt(uint64_t value, uint8_t* buf) {
  size_t n = 0;
  while (value >= 0x80) {
    buf[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  buf[n++] = static_cast<uint8_t>(value);
  return n;
}

// Helper: encode a 4-byte redundant varint (same as ProtoWriter.endNested).
void EncodeRedundantVarInt(uint32_t value, uint8_t* buf) {
  buf[0] = static_cast<uint8_t>((value & 0x7F) | 0x80);
  buf[1] = static_cast<uint8_t>(((value >> 7) & 0x7F) | 0x80);
  buf[2] = static_cast<uint8_t>(((value >> 14) & 0x7F) | 0x80);
  buf[3] = static_cast<uint8_t>((value >> 21) & 0x7F);
}

// Helper: build a TracePacket with a for_testing.payload.str field,
// using the same encoding ProtoWriter would produce.
// Returns the encoded bytes.
std::vector<uint8_t> BuildTestPacket(const std::string& test_string) {
  std::vector<uint8_t> buf;
  buf.reserve(256);

  // TracePacket.for_testing (field 900, wire type 2)
  // for_testing is a nested message
  uint8_t tmp[16];
  size_t n;

  // Tag for field 900, wire type 2 (length-delimited)
  uint32_t tag = (900 << 3) | 2;
  n = EncodeVarInt(tag, tmp);
  buf.insert(buf.end(), tmp, tmp + n);

  // Reserve 4 bytes for outer nested length (redundant varint)
  size_t outer_len_pos = buf.size();
  buf.resize(buf.size() + 4);

  size_t outer_data_start = buf.size();

  // TestEvent.payload (field 5, wire type 2)
  tag = (5 << 3) | 2;
  n = EncodeVarInt(tag, tmp);
  buf.insert(buf.end(), tmp, tmp + n);

  // Reserve 4 bytes for inner nested length
  size_t inner_len_pos = buf.size();
  buf.resize(buf.size() + 4);

  size_t inner_data_start = buf.size();

  // TestEvent.TestPayload.str (field 1, wire type 2)
  tag = (1 << 3) | 2;
  n = EncodeVarInt(tag, tmp);
  buf.insert(buf.end(), tmp, tmp + n);

  // String length
  n = EncodeVarInt(test_string.size(), tmp);
  buf.insert(buf.end(), tmp, tmp + n);

  // String data
  buf.insert(buf.end(), test_string.begin(), test_string.end());

  // Backfill inner nested length
  uint32_t inner_size = static_cast<uint32_t>(buf.size() - inner_data_start);
  EncodeRedundantVarInt(inner_size, buf.data() + inner_len_pos);

  // Backfill outer nested length
  uint32_t outer_size = static_cast<uint32_t>(buf.size() - outer_data_start);
  EncodeRedundantVarInt(outer_size, buf.data() + outer_len_pos);

  return buf;
}

// Helper: write pre-encoded packet bytes to all active instances.
// This is the EXACT same logic as nativeWritePacketToAllInstances in
// perfetto_datasource_jni.cc.
void WritePacketToAllInstances(struct PerfettoDsImpl* ds_impl,
                               const uint8_t* data,
                               size_t len) {
  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    struct PerfettoStreamWriter writer =
        PerfettoDsTracerImplPacketBegin(it.tracer);
    PerfettoStreamWriterAppendBytes(&writer, data, len);
    PerfettoDsTracerImplPacketEnd(it.tracer, &writer);
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }
}

// Helper: check if any instance had incremental state cleared.
// Same logic as nativeCheckAnyIncrementalStateCleared.
bool CheckAnyIncrementalStateCleared(struct PerfettoDsImpl* ds_impl) {
  bool any_cleared = false;
  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    auto* incr = static_cast<IncrState*>(
        PerfettoDsImplGetIncrementalState(ds_impl, it.tracer, it.inst_id));
    if (incr && incr->was_cleared) {
      any_cleared = true;
      incr->was_cleared = false;
    }
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }
  return any_cleared;
}

class JavaDataSourceTest : public testing::Test {
 protected:
  void SetUp() override {
    struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
    args.backends = PERFETTO_BACKEND_IN_PROCESS;
    PerfettoProducerInit(args);

    ds_impl_ = PerfettoDsImplCreate();

    PerfettoDsSetOnCreateIncr(ds_impl_, OnCreateIncr);
    PerfettoDsSetOnDeleteIncr(ds_impl_, OnDeleteIncr);
    PerfettoDsSetOnClearIncr(ds_impl_, OnClearIncr);

    // Build DataSourceDescriptor proto: field 1 = name string
    uint8_t desc[256];
    uint8_t* p = desc;
    *p++ = (1 << 3) | 2;  // field 1, wire type 2
    size_t name_len = strlen(kDataSourceName);
    *p++ = static_cast<uint8_t>(name_len);
    memcpy(p, kDataSourceName, name_len);
    p += name_len;
    size_t desc_size = static_cast<size_t>(p - desc);

    bool ok = PerfettoDsImplRegister(ds_impl_, &enabled_ptr_, desc, desc_size);
    ASSERT_TRUE(ok);
  }

  void TearDown() override {
    perfetto::shlib::ResetForTesting();
    if (ds_impl_) {
      perfetto::shlib::DsImplDestroy(ds_impl_);
      ds_impl_ = nullptr;
    }
  }

  struct PerfettoDsImpl* ds_impl_ = nullptr;
  PERFETTO_ATOMIC(bool) * enabled_ptr_ = nullptr;
};

// Test: disabled data source doesn't execute
TEST_F(JavaDataSourceTest, DisabledNotExecuted) {
  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl_);
  EXPECT_EQ(it.tracer, nullptr);
}

// Test: basic packet write through the JNI path
TEST_F(JavaDataSourceTest, WritePacketViaAppendBytes) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  // Build a packet with for_testing.payload.str = "HELLO_FROM_JAVA"
  std::vector<uint8_t> packet = BuildTestPacket("HELLO_FROM_JAVA");

  // Write it using the same path as the JNI
  WritePacketToAllInstances(ds_impl_, packet.data(), packet.size());

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();

  // Verify: find the for_testing field with our string
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() == 0) {
      continue;
    }
    found = true;
    ASSERT_EQ(for_testing.size(), 1u);
    // Check payload.str
    EXPECT_THAT(FieldView(for_testing.front()),
                ElementsAre(PbField(
                    perfetto_protos_TestEvent_payload_field_number,
                    MsgField(ElementsAre(PbField(
                        perfetto_protos_TestEvent_TestPayload_str_field_number,
                        StringField("HELLO_FROM_JAVA")))))));
  }
  EXPECT_TRUE(found);
}

// Test: multiple packets
TEST_F(JavaDataSourceTest, WriteMultiplePackets) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  std::vector<uint8_t> pkt1 = BuildTestPacket("PACKET_ONE");
  std::vector<uint8_t> pkt2 = BuildTestPacket("PACKET_TWO");
  std::vector<uint8_t> pkt3 = BuildTestPacket("PACKET_THREE");

  WritePacketToAllInstances(ds_impl_, pkt1.data(), pkt1.size());
  WritePacketToAllInstances(ds_impl_, pkt2.data(), pkt2.size());
  WritePacketToAllInstances(ds_impl_, pkt3.data(), pkt3.size());

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();

  int found_count = 0;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() > 0) {
      found_count++;
    }
  }
  EXPECT_EQ(found_count, 3);
}

// Test: incremental state tracking (used for interning)
TEST_F(JavaDataSourceTest, IncrementalStateCleared) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  // First check: state should be cleared (new state)
  bool cleared = CheckAnyIncrementalStateCleared(ds_impl_);
  EXPECT_TRUE(cleared);

  // Second check: state should NOT be cleared (we consumed the flag)
  cleared = CheckAnyIncrementalStateCleared(ds_impl_);
  EXPECT_FALSE(cleared);

  // Write a packet to ensure everything works
  std::vector<uint8_t> pkt = BuildTestPacket("AFTER_INCR_CHECK");
  WritePacketToAllInstances(ds_impl_, pkt.data(), pkt.size());

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();

  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() > 0) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// Test: two concurrent tracing sessions (multi-instance)
TEST_F(JavaDataSourceTest, MultiInstance) {
  TracingSession session1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();
  TracingSession session2 =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  // Count how many instances are active
  int instance_count = 0;
  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl_);
  while (it.tracer) {
    instance_count++;
    PerfettoDsImplTraceIterateNext(ds_impl_, &it);
  }
  EXPECT_EQ(instance_count, 2);

  // Write a packet -- should go to both sessions
  std::vector<uint8_t> pkt = BuildTestPacket("MULTI_INSTANCE");
  WritePacketToAllInstances(ds_impl_, pkt.data(), pkt.size());

  session1.StopBlocking();
  session2.StopBlocking();

  // Both sessions should have the packet
  auto data1 = session1.ReadBlocking();
  auto data2 = session2.ReadBlocking();

  auto count_test_packets = [](const std::vector<uint8_t>& data) -> int {
    int count = 0;
    for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
      IdFieldView for_testing(
          trace_field, perfetto_protos_TracePacket_for_testing_field_number);
      if (for_testing.ok() && for_testing.size() > 0) {
        count++;
      }
    }
    return count;
  };

  EXPECT_EQ(count_test_packets(data1), 1);
  EXPECT_EQ(count_test_packets(data2), 1);
}

// Test: large packet that spans multiple chunks
TEST_F(JavaDataSourceTest, LargePacket) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  // Build a large test string (8KB -- bigger than a typical chunk)
  std::string large_string(8192, 'X');
  std::vector<uint8_t> pkt = BuildTestPacket(large_string);

  WritePacketToAllInstances(ds_impl_, pkt.data(), pkt.size());

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();

  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() > 0) {
      found = true;
      // Verify the payload string matches
      EXPECT_THAT(
          FieldView(for_testing.front()),
          ElementsAre(PbField(
              perfetto_protos_TestEvent_payload_field_number,
              MsgField(ElementsAre(PbField(
                  perfetto_protos_TestEvent_TestPayload_str_field_number,
                  StringField(large_string)))))));
    }
  }
  EXPECT_TRUE(found);
}

}  // namespace
