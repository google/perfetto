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

#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using testing::Invoke;
using testing::MockFunction;

std::string_view ToStringView(const TraceBlobView& tbv) {
  return std::string_view(reinterpret_cast<const char*>(tbv.data()),
                          tbv.size());
}

TEST(ProtoTraceTokenizerTest, TwoPacketsSingleBlob) {
  protozero::HeapBuffered<protozero::Message> message;
  message->AppendString(/*field_id=*/1, "payload1");
  message->AppendString(/*field_id=*/1, "payload2");
  std::vector<uint8_t> data = message.SerializeAsArray();

  MockFunction<base::Status(TraceBlobView)> cb;

  ProtoTraceTokenizer tokenizer;

  EXPECT_CALL(cb, Call)
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload1");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload2");
        return base::OkStatus();
      }));

  auto bv = TraceBlobView(TraceBlob::CopyFrom(data.data(), data.size()));
  EXPECT_TRUE(tokenizer.Tokenize(std::move(bv), cb.AsStdFunction()).ok());
}

TEST(ProtoTraceTokenizerTest, TwoPacketsByteByByte) {
  protozero::HeapBuffered<protozero::Message> message;
  message->AppendString(/*field_id=*/1, "payload1");
  message->AppendString(/*field_id=*/1, "payload2");
  std::vector<uint8_t> data = message.SerializeAsArray();

  ProtoTraceTokenizer tokenizer;

  MockFunction<base::Status(TraceBlobView)> cb;
  EXPECT_CALL(cb, Call)
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload1");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload2");
        return base::OkStatus();
      }));

  for (uint8_t c : data) {
    auto bv = TraceBlobView(TraceBlob::CopyFrom(&c, sizeof(c)));
    EXPECT_TRUE(tokenizer.Tokenize(std::move(bv), cb.AsStdFunction()).ok());
  }
}

TEST(ProtoTraceTokenizerTest, SkipFieldsSingleBlob) {
  protozero::HeapBuffered<protozero::Message> message;
  message->AppendVarInt(/*field_id=*/2, 42);
  message->AppendString(/*field_id=*/1, "payload1");
  message->AppendString(/*field_id=*/3, "ignored");
  message->AppendFixed<uint32_t>(/*field_id=*/3, 42);
  message->AppendString(/*field_id=*/1, "payload2");
  message->AppendFixed<uint64_t>(/*field_id=*/3, 42);
  message->AppendString(/*field_id=*/1, "payload3");
  std::vector<uint8_t> data = message.SerializeAsArray();

  ProtoTraceTokenizer tokenizer;

  MockFunction<base::Status(TraceBlobView)> cb;
  EXPECT_CALL(cb, Call)
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload1");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload2");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload3");
        return base::OkStatus();
      }));

  auto bv = TraceBlobView(TraceBlob::CopyFrom(data.data(), data.size()));
  EXPECT_TRUE(tokenizer.Tokenize(std::move(bv), cb.AsStdFunction()).ok());
}

TEST(ProtoTraceTokenizerTest, SkipFieldsSingleByteByByte) {
  protozero::HeapBuffered<protozero::Message> message;
  message->AppendVarInt(/*field_id=*/2, 42);
  message->AppendString(/*field_id=*/1, "payload1");
  message->AppendString(/*field_id=*/3, "ignored");
  message->AppendFixed<uint32_t>(/*field_id=*/3, 42);
  message->AppendString(/*field_id=*/1, "payload2");
  message->AppendFixed<uint64_t>(/*field_id=*/3, 42);
  message->AppendString(/*field_id=*/1, "payload3");
  std::vector<uint8_t> data = message.SerializeAsArray();

  ProtoTraceTokenizer tokenizer;

  MockFunction<base::Status(TraceBlobView)> cb;
  EXPECT_CALL(cb, Call)
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload1");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload2");
        return base::OkStatus();
      }))
      .WillOnce(Invoke([](TraceBlobView out) {
        EXPECT_EQ(ToStringView(out), "payload3");
        return base::OkStatus();
      }));

  for (uint8_t c : data) {
    auto bv = TraceBlobView(TraceBlob::CopyFrom(&c, sizeof(c)));
    EXPECT_TRUE(tokenizer.Tokenize(std::move(bv), cb.AsStdFunction()).ok());
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
