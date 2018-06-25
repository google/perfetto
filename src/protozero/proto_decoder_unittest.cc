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
#include "src/protozero/scattered_stream_delegate_for_testing.h"

namespace protozero {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using namespace protozero::proto_utils;

TEST(ProtoDecoder, ReadString) {
  Message message;
  perfetto::ScatteredStreamDelegateForTesting delegate(512);
  ScatteredStreamWriter writer(&delegate);
  delegate.set_writer(&writer);
  message.Reset(&writer);

  static constexpr char kTestString[] = "test";
  message.AppendString(1, kTestString);

  uint8_t* data = delegate.chunks()[0].get();
  uint64_t bytes_used = 512 - writer.bytes_available();

  ProtoDecoder decoder(data, bytes_used);
  ProtoDecoder::Field field = decoder.ReadField();

  ASSERT_EQ(field.id, 1u);
  ASSERT_EQ(field.type, proto_utils::FieldType::kFieldTypeLengthDelimited);
  ASSERT_EQ(field.length_limited.length, sizeof(kTestString) - 1);
  for (size_t i = 0; i < sizeof(kTestString) - 1; i++) {
    ASSERT_EQ(field.length_limited.data[i], kTestString[i]);
  }
}

TEST(ProtoDecoder, FixedData) {
  struct FieldExpectation {
    const char* encoded;
    size_t encoded_size;
    uint32_t id;
    FieldType type;
    uint64_t int_value;
  };

  const FieldExpectation kFieldExpectations[] = {
      {"\x08\x00", 2, 1, kFieldTypeVarInt, 0},
      {"\x08\x42", 2, 1, kFieldTypeVarInt, 0x42},
      {"\xF8\x07\x42", 3, 127, kFieldTypeVarInt, 0x42},
      {"\x90\x4D\xFF\xFF\xFF\xFF\x0F", 7, 1234, kFieldTypeVarInt, 0xFFFFFFFF},
      {"\x7D\x42\x00\x00\x00", 5, 15, kFieldTypeFixed32, 0x42},
      {"\x95\x4D\x78\x56\x34\x12", 6, 1234, kFieldTypeFixed32, 0x12345678},
      {"\x79\x42\x00\x00\x00\x00\x00\x00\x00", 9, 15, kFieldTypeFixed64, 0x42},
      {"\x91\x4D\x08\x07\x06\x05\x04\x03\x02\x01", 10, 1234, kFieldTypeFixed64,
       0x0102030405060708},
      {"\x0A\x00", 2, 1, kFieldTypeLengthDelimited, 0},
      {"\x0A\x04|abc", 6, 1, kFieldTypeLengthDelimited, 4},
      {"\x92\x4D\x04|abc", 7, 1234, kFieldTypeLengthDelimited, 4},
      {"\x92\x4D\x83\x01|abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzab"
       "cdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstu"
       "vwx",
       135, 1234, kFieldTypeLengthDelimited, 131},
  };

  for (size_t i = 0; i < perfetto::base::ArraySize(kFieldExpectations); ++i) {
    const FieldExpectation& exp = kFieldExpectations[i];
    ProtoDecoder decoder(reinterpret_cast<const uint8_t*>(exp.encoded),
                         exp.encoded_size);

    ProtoDecoder::Field field = decoder.ReadField();
    ASSERT_EQ(exp.id, field.id);
    ASSERT_EQ(exp.type, field.type);

    if (field.type == kFieldTypeLengthDelimited) {
      ASSERT_EQ(exp.int_value, field.length_limited.length);
    } else {
      ASSERT_EQ(exp.int_value, field.int_value);
    }
  }
}

}  // namespace
}  // namespace protozero
