// Copyright (C) 2022 The Android Open Source Project
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

import {_TextDecoder} from 'custom_utils';

import {defer} from '../../base/deferred';
import {ArrayBufferBuilder} from '../array_buffer_builder';

import {AdbFileHandler} from './adb_file_handler';
import {
  AdbConnection,
  ByteStream,
  OnDisconnectCallback,
  OnMessageCallback,
} from './recording_interfaces_v2';

const textDecoder = new _TextDecoder();

export abstract class AdbConnectionImpl implements AdbConnection {
  // onStatus and onDisconnect are set to callbacks passed from the caller.
  // This happens for instance in the AndroidWebusbTarget, which instantiates
  // them with callbacks passed from the UI.
  onStatus: OnMessageCallback = () => {};
  onDisconnect: OnDisconnectCallback = (_) => {};

  // Starts a shell command, and returns a promise resolved when the command
  // completes.
  async shellAndWaitCompletion(cmd: string): Promise<void> {
    const adbStream = await this.shell(cmd);
    const onStreamingEnded = defer<void>();

    // We wait for the stream to be closed by the device, which happens
    // after the shell command is successfully received.
    adbStream.addOnStreamCloseCallback(() => {
      onStreamingEnded.resolve();
    });
    return onStreamingEnded;
  }

  // Starts a shell command, then gathers all its output and returns it as
  // a string.
  async shellAndGetOutput(cmd: string): Promise<string> {
    const adbStream = await this.shell(cmd);
    const commandOutput = new ArrayBufferBuilder();
    const onStreamingEnded = defer<string>();

    adbStream.addOnStreamDataCallback((data: Uint8Array) => {
      commandOutput.append(data);
    });
    adbStream.addOnStreamCloseCallback(() => {
      onStreamingEnded.resolve(
          textDecoder.decode(commandOutput.toArrayBuffer()));
    });
    return onStreamingEnded;
  }

  async push(binary: Uint8Array, path: string): Promise<void> {
    const byteStream = await this.openStream('sync:');
    await (new AdbFileHandler(byteStream)).pushBinary(binary, path);
    // We need to wait until the bytestream is closed. Otherwise, we can have a
    // race condition:
    // If this is the last stream, it will try to disconnect the device. In the
    // meantime, the caller might create another stream which will try to open
    // the device.
    await byteStream.closeAndWaitForTeardown();
  }

  abstract shell(cmd: string): Promise<ByteStream>;

  abstract canConnectWithoutContention(): Promise<boolean>;

  abstract connectSocket(path: string): Promise<ByteStream>;

  abstract disconnect(disconnectMessage?: string): Promise<void>;

  protected abstract openStream(destination: string): Promise<ByteStream>;
}
