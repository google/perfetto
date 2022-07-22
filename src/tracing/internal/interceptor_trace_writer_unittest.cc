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

#include "perfetto/tracing/internal/interceptor_trace_writer.h"

#include "perfetto/tracing/interceptor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

using ::testing::AllOf;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::IsNull;
using ::testing::MockFunction;
using ::testing::Not;
using ::testing::NotNull;

constexpr uint32_t kInstanceIndex = 42;

}  // namespace

class InterceptorTraceWriterTest : public testing::Test {
 protected:
  using TracePacketCallbackArgs = InterceptorBase::TracePacketCallbackArgs;
  using ThreadLocalState = InterceptorBase::ThreadLocalState;
  using MockTracePacketCallback = MockFunction<void(TracePacketCallbackArgs)>;

  InterceptorTraceWriterTest()
      : tls_ptr_(new ThreadLocalState()),
        tw_(std::unique_ptr<ThreadLocalState>(tls_ptr_),
            TracePacketCallback,
            &dss_,
            kInstanceIndex) {}

  void SetUp() override {
    static_trace_packet_callback_ = &trace_packet_callback_;
  }

  void TearDown() override { static_trace_packet_callback_ = nullptr; }

  static void TracePacketCallback(
      InterceptorBase::TracePacketCallbackArgs args) {
    ASSERT_THAT(static_trace_packet_callback_, NotNull());
    static_trace_packet_callback_->Call(args);
  }

  MockTracePacketCallback trace_packet_callback_;
  static MockTracePacketCallback* static_trace_packet_callback_;

  ThreadLocalState* tls_ptr_;
  DataSourceStaticState dss_;
  InterceptorTraceWriter tw_;
};

InterceptorTraceWriterTest::MockTracePacketCallback*
    InterceptorTraceWriterTest::static_trace_packet_callback_;

TEST_F(InterceptorTraceWriterTest, TracePacketCallbackParams) {
  EXPECT_CALL(trace_packet_callback_,
              Call(AllOf(Field(&TracePacketCallbackArgs::instance_index,
                               kInstanceIndex),
                         Field(&TracePacketCallbackArgs::static_state, &dss_),
                         Field(&TracePacketCallbackArgs::tls, tls_ptr_))))
      .Times(1);

  tw_.NewTracePacket();
  tw_.Flush();
}

TEST_F(InterceptorTraceWriterTest, NewTracePacketAutomaticallyAddedFields) {
  std::string first_packet;
  std::string second_packet;
  EXPECT_CALL(trace_packet_callback_, Call)
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        first_packet = args.packet_data.ToStdString();
      }))
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        second_packet = args.packet_data.ToStdString();
      }));

  tw_.NewTracePacket();
  tw_.NewTracePacket();
  tw_.Flush();

  protos::pbzero::TracePacket::Decoder first(first_packet);
  protos::pbzero::TracePacket::Decoder second(second_packet);
  EXPECT_TRUE(first.has_trusted_packet_sequence_id());
  EXPECT_TRUE(second.has_trusted_packet_sequence_id());
  EXPECT_EQ(first.trusted_packet_sequence_id(),
            second.trusted_packet_sequence_id());
}

TEST_F(InterceptorTraceWriterTest, NewTracePacketLargePacket) {
  size_t first_packet_size;
  size_t second_packet_size;
  EXPECT_CALL(trace_packet_callback_, Call)
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        first_packet_size = args.packet_data.size;
      }))
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        second_packet_size = args.packet_data.size;
      }));

  tw_.NewTracePacket();
  {
    auto msg = tw_.NewTracePacket();
    std::vector<uint8_t> large(20000u, 0);
    msg->AppendRawProtoBytes(large.data(), large.size());
  }
  tw_.Flush();

  EXPECT_EQ(second_packet_size, first_packet_size + 20000u);
}

TEST_F(InterceptorTraceWriterTest, NewTracePacketTakeWriterLargePacket) {
  size_t first_packet_size;
  size_t second_packet_size;
  EXPECT_CALL(trace_packet_callback_, Call)
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        first_packet_size = args.packet_data.size;
      }))
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        second_packet_size = args.packet_data.size;
      }));

  tw_.NewTracePacket();
  tw_.FinishTracePacket();

  protozero::ScatteredStreamWriter* writer =
      tw_.NewTracePacket().TakeStreamWriter();
  std::vector<uint8_t> large(20000u, 0);
  writer->WriteBytes(large.data(), large.size());
  tw_.FinishTracePacket();
  tw_.Flush();

  EXPECT_EQ(second_packet_size, first_packet_size + 20000u);
}

TEST_F(InterceptorTraceWriterTest, MixManualTakeAndMessage) {
  std::string content1 = "AAAAA";
  std::string content2 = "BBBBB";
  std::string content3 = "CCCCC";
  EXPECT_CALL(trace_packet_callback_, Call)
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        std::string data = args.packet_data.ToStdString();
        EXPECT_THAT(data, HasSubstr(content1));
        EXPECT_THAT(data, Not(HasSubstr(content2)));
        EXPECT_THAT(data, Not(HasSubstr(content3)));
      }))
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        std::string data = args.packet_data.ToStdString();
        EXPECT_THAT(data, Not(HasSubstr(content1)));
        EXPECT_THAT(data, HasSubstr(content2));
        EXPECT_THAT(data, Not(HasSubstr(content3)));
      }))
      .WillOnce(Invoke([&](TracePacketCallbackArgs args) {
        std::string data = args.packet_data.ToStdString();
        EXPECT_THAT(data, Not(HasSubstr(content1)));
        EXPECT_THAT(data, Not(HasSubstr(content2)));
        EXPECT_THAT(data, HasSubstr(content3));
      }));

  protozero::ScatteredStreamWriter* writer =
      tw_.NewTracePacket().TakeStreamWriter();
  writer->WriteBytes(reinterpret_cast<const uint8_t*>(content1.data()),
                     content1.size());
  tw_.FinishTracePacket();
  {
    auto msg = tw_.NewTracePacket();
    msg->AppendRawProtoBytes(reinterpret_cast<const uint8_t*>(content2.data()),
                             content2.size());
  }
  writer = tw_.NewTracePacket().TakeStreamWriter();
  writer->WriteBytes(reinterpret_cast<const uint8_t*>(content3.data()),
                     content3.size());
  tw_.FinishTracePacket();

  tw_.Flush();
}

TEST_F(InterceptorTraceWriterTest, FlushCallback) {
  MockFunction<void()> flush_cb;

  InSequence seq;
  EXPECT_CALL(trace_packet_callback_, Call).Times(1);
  EXPECT_CALL(flush_cb, Call).Times(1);

  tw_.NewTracePacket();
  tw_.Flush(flush_cb.AsStdFunction());
}

}  // namespace internal
}  // namespace perfetto
