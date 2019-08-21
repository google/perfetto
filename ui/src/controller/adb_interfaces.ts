// Copyright (C) 2019 The Android Open Source Project
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


export interface Adb {
  connect(device: USBDevice): Promise<void>;
  shell(cmd: string): Promise<AdbStream>;
}

export interface AdbStream {
  onMessage(message: AdbMsg): void;
  onData: (str: string, raw: Uint8Array) => void;
  onConnect: VoidCallback;
  onClose: VoidCallback;
}

export class MockAdb implements Adb {
  connect(_: USBDevice): Promise<void> {
    return Promise.resolve();
  }
  shell(_: string): Promise<AdbStream> {
    return Promise.resolve(new MockAdbStream());
  }
}

export class MockAdbStream implements AdbStream {
  onData = (_: string, __: Uint8Array) => {};
  onConnect = () => {};
  onClose = () => {};
  onMessage = (_: AdbMsg) => {};
}

export declare type CmdType =
    'CNXN' | 'AUTH' | 'CLSE' | 'OKAY' | 'WRTE' | 'OPEN';

export interface AdbMsg {
  cmd: CmdType;
  arg0: number;
  arg1: number;
  data: Uint8Array;
  dataLen: number;
  dataChecksum: number;
}