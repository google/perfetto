/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/protozero/proto_decoder.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"

namespace protozero {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using namespace proto_utils;

TEST(ProtoDecoderTest, ReadString) {
  Message message;
  ScatteredHeapBuffer delegate(512, 512);
  ScatteredStreamWriter writer(&delegate);
  delegate.set_writer(&writer);
  message.Reset(&writer);

  static constexpr char kTestString[] = "test";
  message.AppendString(1, kTestString);

  delegate.AdjustUsedSizeOfCurrentSlice();
  auto used_range = delegate.slices()[0].GetUsedRange();

  TypedProtoDecoder<32, false> decoder(used_range.begin, used_range.size());

  const auto& field = decoder.Get(1);
  ASSERT_EQ(field.type(), ProtoWireType::kLengthDelimited);
  ASSERT_EQ(field.size(), sizeof(kTestString) - 1);
  for (size_t i = 0; i < sizeof(kTestString) - 1; i++) {
    ASSERT_EQ(field.data()[i], kTestString[i]);
  }
}

TEST(ProtoDecoderTest, VeryLargeField) {
  const uint64_t size = 512 * 1024 * 1024 + 6;
  std::unique_ptr<uint8_t, perfetto::base::FreeDeleter> data(
      static_cast<uint8_t*>(malloc(size)));

  data.get()[0] = static_cast<unsigned char>('\x0A');
  data.get()[1] = static_cast<unsigned char>('\x80');
  data.get()[2] = static_cast<unsigned char>('\x80');
  data.get()[3] = static_cast<unsigned char>('\x80');
  data.get()[4] = static_cast<unsigned char>('\x80');
  data.get()[5] = static_cast<unsigned char>('\x02');

  ProtoDecoder decoder(data.get(), size);
  Field field = decoder.ReadField();
  ASSERT_EQ(1u, field.id());
  ASSERT_EQ(nullptr, field.data());
  ASSERT_EQ(0u, field.size());
  ASSERT_EQ(0u, decoder.bytes_left());
}

TEST(ProtoDecoderTest, FixedData) {
  struct FieldExpectation {
    const char* encoded;
    size_t encoded_size;
    uint32_t id;
    ProtoWireType type;
    uint64_t int_value;
  };

  const FieldExpectation kFieldExpectations[] = {
      {"\x08\x00", 2, 1, ProtoWireType::kVarInt, 0},
      {"\x08\x01", 2, 1, ProtoWireType::kVarInt, 1},
      {"\x08\x42", 2, 1, ProtoWireType::kVarInt, 0x42},
      {"\xF8\x07\x42", 3, 127, ProtoWireType::kVarInt, 0x42},
      {"\xB8\x3E\xFF\xFF\xFF\xFF\x0F", 7, 999, ProtoWireType::kVarInt,
       0xFFFFFFFF},
      {"\x7D\x42\x00\x00\x00", 5, 15, ProtoWireType::kFixed32, 0x42},
      {"\xBD\x3E\x78\x56\x34\x12", 6, 999, ProtoWireType::kFixed32, 0x12345678},
      {"\x79\x42\x00\x00\x00\x00\x00\x00\x00", 9, 15, ProtoWireType::kFixed64,
       0x42},
      {"\xB9\x3E\x08\x07\x06\x05\x04\x03\x02\x01", 10, 999,
       ProtoWireType::kFixed64, 0x0102030405060708},
      {"\x0A\x00", 2, 1, ProtoWireType::kLengthDelimited, 0},
      {"\x0A\x04|abc", 6, 1, ProtoWireType::kLengthDelimited, 4},
      {"\xBA\x3E\x04|abc", 7, 999, ProtoWireType::kLengthDelimited, 4},
      {"\xBA\x3E\x83\x01|abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzab"
       "cdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstu"
       "vwx",
       135, 999, ProtoWireType::kLengthDelimited, 131},
  };

  for (size_t i = 0; i < perfetto::base::ArraySize(kFieldExpectations); ++i) {
    const FieldExpectation& exp = kFieldExpectations[i];
    TypedProtoDecoder<999, 0> decoder(
        reinterpret_cast<const uint8_t*>(exp.encoded), exp.encoded_size);

    auto& field = decoder.Get(exp.id);
    ASSERT_EQ(exp.type, field.type());

    if (field.type() == ProtoWireType::kLengthDelimited) {
      ASSERT_EQ(exp.int_value, field.size());
    } else {
      ASSERT_EQ(exp.int_value, field.as_int64());
      // Proto encodes booleans as varints of 0 or 1.
      if (exp.int_value == 0 || exp.int_value == 1) {
        ASSERT_EQ(exp.int_value, field.as_bool());
      }
    }
  }

  // Test float and doubles decoding.
  const char buf[] = "\x0d\x00\x00\xa0\x3f\x11\x00\x00\x00\x00\x00\x42\x8f\xc0";
  TypedProtoDecoder<2, 0> decoder(reinterpret_cast<const uint8_t*>(buf),
                                  sizeof(buf));
  EXPECT_FLOAT_EQ(decoder.Get(1).as_float(), 1.25f);
  EXPECT_DOUBLE_EQ(decoder.Get(2).as_double(), -1000.25);
}

}  // namespace
}  // namespace protozero
