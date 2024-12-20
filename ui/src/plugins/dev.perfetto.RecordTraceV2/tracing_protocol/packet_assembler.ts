// Copyright (C) 2024 The Android Open Source Project
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

import protos from '../../../protos';
import {ResizableArrayBuffer} from '../../../base/resizable_array_buffer';
import {exists} from '../../../base/utils';

/**
 * Utility class to re-assemble trace packets from slice fragments.
 * This is needed to deal with ReadBuffersResponse. Each ReadBuffersResponse
 * provies an array of slices. A slice can be == a packet, or a fragment of it.
 * Furthermore each ReadBufferResponse can provide slices for >1 packet (or for
 * a packet and a bit). This class deals with the reassembly.
 */
export class PacketAssembler {
  // Buffers the incoming slices until we see a full packet.
  private curPacketSlices = new Array<Uint8Array>();

  /**
   * @param rdResp a ReadBufferResponse containing an array of slices.
   * @returns A protos.perfetto.Trace protobuf-encoded buffer containing a
   * sequence of whole packets. This buffer is suitable to be pushed into
   * TraceProcessor, traceconv or other perfetto tools.
   */
  pushSlices(rdResp: protos.IReadBuffersResponse): Uint8Array {
    const traceBuf = new ResizableArrayBuffer(4096);
    for (const slice of rdResp.slices ?? []) {
      if (!exists(slice.data)) continue;
      this.curPacketSlices.push(slice.data);
      if (!Boolean(slice.lastSliceForPacket)) {
        continue;
      }

      // We received all the slices for the current packet.
      // Below we assemble all the slices for each packet together and
      // prepend them with the proto preamble.
      const slices = this.curPacketSlices.splice(0); // ps = std::move(this.ps).

      // We receive 1+ slices per packet. The slices contain only the payload
      // of the packet, but not the packet preamble itself. We have to write
      // the packet proto preamble ourselves. In order to do so we need to first
      // compute the total packet size.
      const totLen = slices.reduce((a, buf) => a + buf.length, 0);

      // Becuase the packet size is varint-encoded, we don't know how many bytes
      // the premable is going to take. Allow for 10 bytes of preamble. We will
      // subarray() to the actual length at the end of this function.
      const preamble: number[] = [TRACE_PACKET_PROTO_TAG];
      let lenVarint = totLen;
      do {
        preamble.push((lenVarint & 0x7f) | (lenVarint > 0x7f ? 0x80 : 0));
        lenVarint >>>= 7;
      } while (lenVarint > 0);
      traceBuf.append(preamble);
      slices.forEach((slice) => traceBuf.append(slice));
    } // for(slices)
    return traceBuf.get();
  }
}

const PROTO_LEN_DELIMITED_WIRE_TYPE = 2;
const TRACE_PACKET_PROTO_ID = 1;
const TRACE_PACKET_PROTO_TAG =
  (TRACE_PACKET_PROTO_ID << 3) | PROTO_LEN_DELIMITED_WIRE_TYPE;
