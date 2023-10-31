// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {assertTrue} from '../base/logging';

// This class is the TypeScript equivalent of the identically-named C++ class in
// //protozero/proto_ring_buffer.h. See comments in that header for a detailed
// description. The architecture is identical.

const kGrowBytes = 128 * 1024;
const kMaxMsgSize = 64 * 1024 * 1024;

export class ProtoRingBuffer {
  private buf = new Uint8Array(kGrowBytes);
  private fastpath?: Uint8Array;
  private rd = 0;
  private wr = 0;

  // The caller must call ReadMessage() after each append() call.
  // The |data| might be either copied in the internal ring buffer or returned
  // (% subarray()) to the next ReadMessage() call.
  append(data: Uint8Array) {
    assertTrue(this.wr <= this.buf.length);
    assertTrue(this.rd <= this.wr);

    // If the last call to ReadMessage() consumed all the data in the buffer and
    // there are no incomplete messages pending, restart from the beginning
    // rather than keep ringing. This is the most common case.
    if (this.rd === this.wr) {
      this.rd = this.wr = 0;
    }

    // The caller is expected to issue a ReadMessage() after each append().
    const dataLen = data.length;
    if (dataLen === 0) return;
    assertTrue(this.fastpath === undefined);
    if (this.rd === this.wr) {
      const msg = ProtoRingBuffer.tryReadMessage(data, 0, dataLen);
      if (msg !== undefined &&
          ((msg.byteOffset + msg.length) === (data.byteOffset + dataLen))) {
        // Fastpath: in many cases, the underlying stream will effectively
        // preserve the atomicity of messages for most small messages.
        // In this case we can avoid the extra buffer roundtrip and return the
        // original array (actually a subarray that skips the proto header).
        // The next call to ReadMessage() will return this.
        this.fastpath = msg;
        return;
      }
    }

    let avail = this.buf.length - this.wr;
    if (dataLen > avail) {
      // This whole section should be hit extremely rarely.

      // Try first just recompacting the buffer by moving everything to the
      // left. This can happen if we received "a message and a bit" on each
      // append() call.
      this.buf.copyWithin(0, this.rd, this.wr);
      avail += this.rd;
      this.wr -= this.rd;
      this.rd = 0;
      if (dataLen > avail) {
        // Still not enough, expand the buffer.
        let newSize = this.buf.length;
        while (dataLen > newSize - this.wr) {
          newSize += kGrowBytes;
        }
        assertTrue(newSize <= kMaxMsgSize * 2);
        const newBuf = new Uint8Array(newSize);
        newBuf.set(this.buf);
        this.buf = newBuf;
        // No need to touch rd / wr.
      }
    }

    // Append the received data at the end of the ring buffer.
    this.buf.set(data, this.wr);
    this.wr += dataLen;
  }

  // Tries to extract a message from the ring buffer. If there is no message,
  // or if the current message is still incomplete, returns undefined.
  // The caller is expected to call this in a loop until it returns undefined.
  // Note that a single write to Append() can yield more than one message
  // (see ProtoRingBufferTest.CoalescingStream in the unittest).
  readMessage(): Uint8Array|undefined {
    if (this.fastpath !== undefined) {
      assertTrue(this.rd === this.wr);
      const msg = this.fastpath;
      this.fastpath = undefined;
      return msg;
    }
    assertTrue(this.rd <= this.wr);
    if (this.rd >= this.wr) {
      return undefined;  // Completely empty.
    }
    const msg = ProtoRingBuffer.tryReadMessage(this.buf, this.rd, this.wr);
    if (msg === undefined) return undefined;
    assertTrue(msg.buffer === this.buf.buffer);
    assertTrue(this.buf.byteOffset === 0);
    this.rd = msg.byteOffset + msg.length;

    // Deliberately returning a copy of the data with slice(). In various cases
    // (streaming query response) the caller will hold onto the returned buffer.
    // If we get to this point, |msg| is a view of the circular buffer that we
    // will overwrite on the next calls to append().
    return msg.slice();
  }

  private static tryReadMessage(
      data: Uint8Array, dataStart: number, dataEnd: number): Uint8Array
      |undefined {
    assertTrue(dataEnd <= data.length);
    let pos = dataStart;
    if (pos >= dataEnd) return undefined;
    const tag = data[pos++];  // Assume one-byte tag.
    if (tag >= 0x80 || (tag & 0x07) !== 2 /* len delimited */) {
      throw new Error(
          `RPC framing error, unexpected tag ${tag} @ offset ${pos - 1}`);
    }

    let len = 0;
    for (let shift = 0; /* no check */; shift += 7) {
      if (pos >= dataEnd) {
        return undefined;  // Not enough data to read varint.
      }
      const val = data[pos++];
      len |= ((val & 0x7f) << shift) >>> 0;
      if (val < 0x80) break;
    }

    if (len >= kMaxMsgSize) {
      throw new Error(
          `RPC framing error, message too large (${len} > ${kMaxMsgSize}`);
    }
    const end = pos + len;
    if (end > dataEnd) return undefined;

    // This is a subarray() and not a slice() because in the |fastpath| case
    // we want to just return the original buffer pushed by append().
    // In the slow-path (ring-buffer) case, the readMessage() above will create
    // a copy via slice() before returning it.
    return data.subarray(pos, end);
  }
}
