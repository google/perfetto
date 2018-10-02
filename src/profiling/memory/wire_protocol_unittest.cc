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

#include "src/profiling/memory/wire_protocol.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/record_reader.h"

#include <sys/socket.h>
#include <sys/types.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {

bool operator==(const AllocMetadata& one, const AllocMetadata& other);
bool operator==(const AllocMetadata& one, const AllocMetadata& other) {
  return std::tie(one.sequence_number, one.alloc_size, one.alloc_address,
                  one.stack_pointer, one.stack_pointer_offset, one.arch) ==
             std::tie(other.sequence_number, other.alloc_size,
                      other.alloc_address, other.stack_pointer,
                      other.stack_pointer_offset, other.arch) &&
         memcmp(one.register_data, other.register_data, kMaxRegisterDataSize) ==
             0;
}

bool operator==(const FreeMetadata& one, const FreeMetadata& other);
bool operator==(const FreeMetadata& one, const FreeMetadata& other) {
  if (one.num_entries != other.num_entries)
    return false;
  for (size_t i = 0; i < one.num_entries; ++i) {
    if (std::tie(one.entries[i].sequence_number, one.entries[i].addr) !=
        std::tie(other.entries[i].sequence_number, other.entries[i].addr))
      return false;
  }
  return true;
}

namespace {

RecordReader::Record ReceiveAll(int sock) {
  RecordReader record_reader;
  RecordReader::Record record;
  bool received = false;
  while (!received) {
    RecordReader::ReceiveBuffer buf = record_reader.BeginReceive();
    ssize_t rd = PERFETTO_EINTR(read(sock, buf.data, buf.size));
    PERFETTO_CHECK(rd > 0);
    auto status = record_reader.EndReceive(static_cast<size_t>(rd), &record);
    switch (status) {
      case (RecordReader::Result::Noop):
        break;
      case (RecordReader::Result::RecordReceived):
        received = true;
        break;
      case (RecordReader::Result::KillConnection):
        PERFETTO_CHECK(false);
        break;
    }
  }
  return record;
}

TEST(WireProtocolTest, AllocMessage) {
  char payload[] = {0x77, 0x77, 0x77, 0x00};
  WireMessage msg = {};
  msg.record_type = RecordType::Malloc;
  AllocMetadata metadata = {};
  metadata.sequence_number = 0xA1A2A3A4A5A6A7A8;
  metadata.alloc_size = 0xB1B2B3B4B5B6B7B8;
  metadata.alloc_address = 0xC1C2C3C4C5C6C7C8;
  metadata.stack_pointer = 0xD1D2D3D4D5D6D7D8;
  metadata.stack_pointer_offset = 0xE1E2E3E4E5E6E7E8;
  metadata.arch = unwindstack::ARCH_X86;
  for (size_t i = 0; i < kMaxRegisterDataSize; ++i)
    metadata.register_data[i] = 0x66;
  msg.alloc_header = &metadata;
  msg.payload = payload;
  msg.payload_size = sizeof(payload);

  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  base::ScopedFile send_sock(sv[0]);
  base::ScopedFile recv_sock(sv[1]);
  ASSERT_TRUE(SendWireMessage(*send_sock, msg));

  RecordReader::Record record = ReceiveAll(*recv_sock);

  WireMessage recv_msg;
  ASSERT_TRUE(ReceiveWireMessage(reinterpret_cast<char*>(record.data.get()),
                                 record.size, &recv_msg));
  ASSERT_EQ(recv_msg.record_type, msg.record_type);
  ASSERT_EQ(*recv_msg.alloc_header, *msg.alloc_header);
  ASSERT_EQ(recv_msg.payload_size, msg.payload_size);
  ASSERT_STREQ(recv_msg.payload, msg.payload);
}

TEST(WireProtocolTest, FreeMessage) {
  WireMessage msg = {};
  msg.record_type = RecordType::Free;
  FreeMetadata metadata = {};
  metadata.num_entries = kFreePageSize;
  for (size_t i = 0; i < kFreePageSize; ++i) {
    metadata.entries[i].sequence_number = 0x111111111111111;
    metadata.entries[i].addr = 0x222222222222222;
  }
  msg.free_header = &metadata;

  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  base::ScopedFile send_sock(sv[0]);
  base::ScopedFile recv_sock(sv[1]);
  ASSERT_TRUE(SendWireMessage(*send_sock, msg));

  RecordReader::Record record = ReceiveAll(*recv_sock);

  WireMessage recv_msg;
  ASSERT_TRUE(ReceiveWireMessage(reinterpret_cast<char*>(record.data.get()),
                                 record.size, &recv_msg));
  ASSERT_EQ(recv_msg.record_type, msg.record_type);
  ASSERT_EQ(*recv_msg.free_header, *msg.free_header);
  ASSERT_EQ(recv_msg.payload_size, msg.payload_size);
}

}  // namespace
}  // namespace perfetto
