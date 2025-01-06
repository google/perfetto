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

import {defer} from '../../../base/deferred';
import {ResizableArrayBuffer} from '../../../base/resizable_array_buffer';
import {okResult, Result} from '../../../base/result';
import {utf8Decode} from '../../../base/string_utils';
import {ByteStream} from '../interfaces/byte_stream';

/**
 * A base abstraction that represents an Android ADB device, allowing to shell
 * commands and create streams (e.g. connecting to a UNIX-socket).
 * This abstraction exists so that AdbTracingSession can drive a tracing session
 * regardless of the underlying Webusb Websocket connection.
 * AdbWebusbDevice and AdbWebsocketDevice implement this.
 * @see @class AdbWebusbDevice
 * @see @class AdbWebsocketDevice
 */
export abstract class AdbDevice {
  /**
   * Opens an ADB Stream. Example services:
   * - 'shell:command arg1 arg2 ...'
   * - 'shell:' for interactive shell
   * - 'localfilesystem:/dev/socket/xxx': for UNIX sockets.
   * - 'localabstract:sock_name': for UNIX abstract sockets.
   */
  abstract createStream(svc: string): Promise<Result<ByteStream>>;

  abstract close(): void;

  /** Invoke a command and return its stdout+err. */
  async shell(cmd: string): Promise<Result<string>> {
    const cmdOut = new ResizableArrayBuffer();
    const streamEndedPromise = defer<string>();
    const status = await this.createStream(`shell:${cmd}`);
    if (!status.ok) return status;
    const stream = status.value;
    stream.onData = (data: Uint8Array) => cmdOut.append(data);
    stream.onClose = () => {
      streamEndedPromise.resolve(utf8Decode(cmdOut.get()));
    };
    const outTxt = (await streamEndedPromise).trimEnd();
    return okResult(outTxt);
  }
}
