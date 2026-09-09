/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_PROTOZERO_PROTO_RING_BUFFER_H_
#define INCLUDE_PERFETTO_EXT_PROTOZERO_PROTO_RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/base/compiler.h"

namespace protozero {

// This class buffers and tokenizes proto messages.
//
// From a logical level, it works with a sequence of protos like this.
// [ header 1 ] [ payload 1   ]
// [ header 2 ] [ payload 2  ]
// [ header 3 ] [ payload 3     ]
// Where [ header ] is a variable-length sequence of:
// [ Field ID = 1, type = length-delimited] [ length (varint) ].
//
// The input to this class is byte-oriented, not message-oriented (like a TCP
// stream or a pipe). The caller is not required to respect the boundaries of
// each message; only guarantee that data is not lost or duplicated. The
// following sequence of inbound events is possible:
// 1. [ hdr 1 (incomplete) ... ]
// 2. [ ... hdr 1 ] [ payload 1 ] [ hdr 2 ] [ payoad 2 ] [ hdr 3 ] [ pay... ]
// 3. [ ...load 3 ]
//
// This class maintains inbound requests in a ring buffer.
// The expected usage is:
// auto write = ring_buf.BeginWrite(len);
// write.EndWrite(read(fd, write.data(), write.size()));
// for (;;) {
//   auto msg = ring_buf.ReadMessage();
//   if (!msg.valid())
//     break;
//   Decode(msg);
// }
//
// After each write, the caller is expected to call ReadMessage() until
// it returns an invalid message (signalling no more messages could be decoded).
// Note that a single write can "unblock" > 1 messages, which is why the caller
// needs to keep calling ReadMessage in a loop.
//
// Internal architecture
// ---------------------
// Internally this is similar to a ring-buffer, with the caveat that it never
// wraps, it only expands. Expansions are rare. The deal is that in most cases
// the read cursor follows very closely the write cursor. For instance, if the
// underlying transport behaves as a dgram socket, after each write, the read
// cursor will chase completely the write cursor. Even if the underlying stream
// is not always atomic, the expectation is that the read cursor will eventually
// reach the write one within few messages.
// A visual example, imagine we have four messages: 2it 4will 2be 4fine
// Visually:
//
// Write("2it4wi"): A message and a bit:
// [ 2it 4wi                     ]
// ^R       ^W
//
// After the ReadMessage(), the 1st message will be read, but not the 2nd.
// [ 2it 4wi                     ]
//      ^R ^W
//
// Write("ll2be4f")
// [ 2it 4will 2be 4f            ]
//      ^R           ^W
//
// After the ReadMessage() loop:
// [ 2it 4will 2be 4f            ]
//                ^R ^W
// Write("ine")
// [ 2it 4will 2be 4fine         ]
//                ^R    ^W
//
// In the next ReadMessage() the R cursor will chase the W cursor. When this
// happens (very frequent) we can just reset both cursors to 0 and restart.
// If we are unlucky and get to the end of the buffer, two things happen:
// 1. We try first to recompact the buffer, moving everything left by R.
// 2. If still there isn't enough space, we expand the buffer.
// Given that each message is expected to be at most kMaxMsgSize (64 MB), the
// expansion is bound at 2 * kMaxMsgSize.
//
// All of the above is only possible while no Message still points into the
// buffer. Messages hold a reference to it, so if the caller retains one (e.g.
// to hand it to another thread) the next write moves onto another buffer and
// the last Message releases the old one. The buffer left behind is picked up
// again by the following switch, so retaining a message per write ping-pongs
// between two buffers rather than allocating on each of them.

class ProtoRingBuffer {
 private:
  class Buffer;

 public:
  static constexpr size_t kMaxMsgSize = 64 * 1024 * 1024;
  // The payload of one field, without its preamble. Keeps the buffer it points
  // into alive, so it stays valid across further writes and can be moved to
  // another thread.
  class Message {
   public:
    Message() = default;
    ~Message();
    Message(Message&&) noexcept;
    Message& operator=(Message&&) noexcept;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    const uint8_t* data() const { return start_; }
    const uint8_t* end() const { return start_ + len_; }
    uint32_t size() const { return len_; }
    uint32_t field_id() const { return field_id_; }
    bool valid() const { return !!start_; }
    bool fatal_framing_error() const { return fatal_framing_error_; }

   private:
    friend class ProtoRingBuffer;
    Buffer* buf_ = nullptr;
    const uint8_t* start_ = nullptr;
    uint32_t len_ = 0;
    uint32_t field_id_ = 0;
    bool fatal_framing_error_ = false;
  };

  ProtoRingBuffer();
  ~ProtoRingBuffer();
  ProtoRingBuffer(const ProtoRingBuffer&) = delete;
  ProtoRingBuffer& operator=(const ProtoRingBuffer&) = delete;

  // Returned by BeginWrite() and must be given back to EndWrite() (to keep
  // what was written) or AbortWrite() (to discard it). It enforces that
  // writes are neither left pending nor interleaved: only one handle can
  // exist at a time, and letting it go out of scope without passing it to
  // EndWrite()/AbortWrite() causes a CHECK. The buffer will not recompact or
  // grow while a handle is outstanding.
  class WriteHandle {
   public:
    WriteHandle() = default;
    ~WriteHandle();

    WriteHandle(WriteHandle&&) noexcept;
    WriteHandle& operator=(WriteHandle&&) noexcept;
    WriteHandle(const WriteHandle&) = delete;
    WriteHandle& operator=(const WriteHandle&) = delete;

    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

    void EndWrite(size_t size_written);
    void AbortWrite();

    explicit operator bool() const { return buffer_ != nullptr; }

   private:
    friend class ProtoRingBuffer;
    WriteHandle(ProtoRingBuffer* buffer, uint8_t* data, size_t size)
        : buffer_(buffer), data_(data), size_(size) {}

    ProtoRingBuffer* buffer_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
  };

  // The caller reads straight into the reservation (e.g. by passing data() to
  // read(2)).
  PERFETTO_WARN_UNUSED_RESULT WriteHandle BeginWrite(size_t size);

  // If a message can be read, it returns the boundaries of the message
  // (without including the preamble) and advances the read cursor.
  // If no message is available, returns a null range.
  Message ReadMessage();

  // Exposed for testing: buffers allocated over the lifetime of the reader.
  uint32_t num_buffers_for_testing() const { return num_buffers_; }

 private:
  friend class WriteHandle;

  void SwitchToFreshBuffer(size_t capacity);
  void FinishWrite(size_t size_written);

  // Refcounted; the ring buffer holds one reference, each Message another.
  Buffer* buf_ = nullptr;
  // The buffer left behind by the last SwitchToFreshBuffer(), reused once the
  // Messages pinning it are released.
  Buffer* spare_ = nullptr;
  bool failed_ = false;  // Set in case of an unrecoverable framing faiulre.
  size_t rd_ = 0;        // Offset of the read cursor in |buf_|.
  size_t wr_ = 0;        // Offset of the write cursor in |buf_|.
  bool write_in_flight_ = false;
  uint32_t num_buffers_ = 1;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_EXT_PROTOZERO_PROTO_RING_BUFFER_H_
