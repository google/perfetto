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

import {ByteStream} from '../../interfaces/byte_stream';
import {AdbWebusbDevice} from './adb_webusb_device';

export class AdbWebusbStream extends ByteStream {
  private state: 'CONNECTED' | 'CLOSING' | 'CLOSED' = 'CONNECTED';

  constructor(
    private adbWebusbDevice: AdbWebusbDevice,
    readonly localId: number,
    readonly remoteId: number,
  ) {
    super();
  }

  get connected(): boolean {
    return this.state === 'CONNECTED';
  }

  write(data: string | Uint8Array): Promise<void> {
    if (this.state !== 'CONNECTED') {
      // Ignore writes queued once the stream is being closed.
      return Promise.resolve();
    }
    return this.adbWebusbDevice.streamWrite(this, data);
  }

  // This is invoked by the user to request closure. This is the case when the
  // closure is initiated by the caller (e.g. terminating a shell process).
  close(): void {
    if (this.state !== 'CONNECTED') return;
    this.state = 'CLOSING';
    this.adbWebusbDevice.streamClose(this);
  }

  // Called by AdbWebusbTransport in two cases:
  // 1. To ACK a closure request, if we are in state = 'CLOSING'.
  // 2. To inform us about device-side closure (e.g. the process terminated)
  //    if we are in state 'CONNECTED'.
  notifyClose() {
    if (this.state === 'CLOSED') return;
    this.state = 'CLOSED';
    this.onClose();
  }
}
