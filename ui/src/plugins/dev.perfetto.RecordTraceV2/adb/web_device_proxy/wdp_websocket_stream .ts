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

import z from 'zod';
import {assertExists, assertTrue} from '../../../../base/assert';
import {ByteStream} from '../../interfaces/byte_stream';
import {base64Decode} from '../../../../base/string_utils';

export class WdpWebSocketStream extends ByteStream {
  private schema = z.object({
    response: z.string(),
  });

  constructor(private sock: WebSocket) {
    super();
    sock.binaryType = 'arraybuffer';
    sock.onclose = () => this.onClose();
    sock.onmessage = async (e: MessageEvent) => {
      if (typeof e.data === 'string') {
        // On Windows, the public version of WebDeviceProxy unfortunately
        // predates cl/706026527 which was responsbile for sending adb frames
        // as binary. This means it sends it as a protojson message with the
        // schema:
        // {
        //   "response": "<base64 encoded bytes>"
        // }
        //
        // Unmarshall this transparently to caller.
        const parsed = this.schema.safeParse(JSON.parse(e.data));
        this.onData(
          new Uint8Array(base64Decode(assertExists(parsed.data).response)),
        );
      } else {
        assertTrue(e.data instanceof ArrayBuffer);
        this.onData(new Uint8Array(e.data as ArrayBuffer));
      }
    };
  }

  get connected(): boolean {
    return this.sock.readyState === WebSocket.OPEN;
  }

  async write(data: string | Uint8Array): Promise<void> {
    this.sock.send(data);
  }

  close(): void {
    this.sock.close();
  }
}
