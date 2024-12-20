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

import {assertTrue} from '../../../base/logging';
import {ByteStream} from '../interfaces/byte_stream';

export class WebSocketStream extends ByteStream {
  constructor(private sock: WebSocket) {
    super();
    sock.binaryType = 'arraybuffer';
    sock.onclose = () => this.onClose();
    sock.onmessage = async (e: MessageEvent) => {
      assertTrue(e.data instanceof ArrayBuffer);
      this.onData(new Uint8Array(e.data as ArrayBuffer));
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
