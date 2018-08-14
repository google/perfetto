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

#include "src/profiling/memory/record_reader.h"

#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(RecordReaderTest, ZeroLengthRecord) {
  RecordReader record_reader;
  uint64_t size = 0;
  RecordReader::ReceiveBuffer buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t));
  memcpy(buf.data, &size, sizeof(size));
  RecordReader::Record record;
  ASSERT_EQ(record_reader.EndReceive(sizeof(size), &record),
            RecordReader::Result::RecordReceived);
  ASSERT_EQ(record.size, 0);
}

TEST(RecordReaderTest, OneRecord) {
  RecordReader record_reader;
  uint64_t size = 1;
  RecordReader::ReceiveBuffer buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t));
  memcpy(buf.data, &size, sizeof(size));
  RecordReader::Record record;
  ASSERT_EQ(record_reader.EndReceive(sizeof(size), &record),
            RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, 1);
  memcpy(buf.data, "1", 1);
  ASSERT_EQ(record_reader.EndReceive(1, &record),
            RecordReader::Result::RecordReceived);
  ASSERT_EQ(record.size, 1);
}

TEST(RecordReaderTest, OneRecordPartialSize) {
  RecordReader record_reader;
  uint64_t size = 1;
  RecordReader::ReceiveBuffer buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t));
  memcpy(buf.data, &size, sizeof(size) / 2);
  RecordReader::Record record;
  ASSERT_EQ(record_reader.EndReceive(sizeof(size) / 2, &record),
            RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t) / 2);
  memcpy(buf.data, reinterpret_cast<uint8_t*>(&size) + sizeof(size) / 2,
         sizeof(size) / 2);
  ASSERT_EQ(record_reader.EndReceive(sizeof(size) / 2, &record),
            RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, 1);
  memcpy(buf.data, "1", 1);
  ASSERT_EQ(record_reader.EndReceive(1, &record),
            RecordReader::Result::RecordReceived);
  ASSERT_EQ(record.size, 1);
}

TEST(RecordReaderTest, TwoRecords) {
  RecordReader record_reader;
  uint64_t size = 1;
  RecordReader::ReceiveBuffer buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t));
  memcpy(buf.data, &size, sizeof(size));
  RecordReader::Record record;
  ASSERT_EQ(record_reader.EndReceive(sizeof(size), &record),
            RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, 1);
  memcpy(buf.data, "1", 1);
  ASSERT_EQ(record_reader.EndReceive(1, &record),
            RecordReader::Result::RecordReceived);
  ASSERT_EQ(record.size, 1);

  size = 2;
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, sizeof(uint64_t));
  memcpy(buf.data, &size, sizeof(size));
  ASSERT_EQ(record_reader.EndReceive(sizeof(size), &record),
            RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, 2);
  memcpy(buf.data, "1", 1);
  ASSERT_EQ(record_reader.EndReceive(1, &record), RecordReader::Result::Noop);
  buf = record_reader.BeginReceive();
  ASSERT_EQ(buf.size, 1);
  memcpy(buf.data, "2", 1);
  ASSERT_EQ(record_reader.EndReceive(1, &record),
            RecordReader::Result::RecordReceived);
  ASSERT_EQ(record.size, 2);
  ASSERT_EQ(record.data[0], '1');
  ASSERT_EQ(record.data[1], '2');
}

}  // namespace
}  // namespace perfetto
