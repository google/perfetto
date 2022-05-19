#include "perfetto/tracing/internal/interceptor_trace_writer.h"

#include "perfetto/tracing/interceptor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

using ::testing::AllOf;
using ::testing::Field;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::IsNull;
using ::testing::MockFunction;
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
