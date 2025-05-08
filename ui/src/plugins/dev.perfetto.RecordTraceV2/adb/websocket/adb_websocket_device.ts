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

import {Result, errResult, okResult} from '../../../../base/result';
import {WebSocketStream} from '../../websocket/websocket_stream';
import {AdbDevice} from '../adb_device';
import {adbCmdAndWait} from './adb_websocket_utils';
import {AsyncWebsocket} from '../../websocket/async_websocket';

export type AdbWebsocketMode = 'WEBSOCKET_BRIDGE' | 'WEB_DEVICE_PROXY';
/**
 * This class implements the state machine required to communicate with an ADB
 * device over WebSocket using either the Perfetto websocket_bridge or the
 * Google Web Device Proxy. The two are almost identical with the exception of
 * the initial handshake.
 * It takes a websocket url as input (which behind the scenes is a plain
 * bridge to adbd TCP on 127.0.0.1:5037) and a device serial and returns an
 * object suitable to run shell commands and create streams on it.
 */
export class AdbWebsocketDevice extends AdbDevice {
  private streams = new Array<WebSocketStream>();

  private constructor(
    private wsUrl: string,
    private deviceSerial: string,
    // This socket is only used to tell if we are still connected or not.
    // Each stream needs a new websocket because of the way the ADB TCP protocol
    // works.
    private transportSock: AsyncWebsocket,
    private mode: AdbWebsocketMode,
  ) {
    super();
  }

  static async connect(
    wsUrl: string,
    deviceSerial: string,
    mode: AdbWebsocketMode,
  ): Promise<Result<AdbWebsocketDevice>> {
    const status = await this.connectToTransport(wsUrl, deviceSerial, mode);
    if (!status.ok) return status;
    const sock = status.value;
    return okResult(new AdbWebsocketDevice(wsUrl, deviceSerial, sock, mode));
  }

  private static async connectToTransport(
    wsUrl: string,
    deviceSerial: string,
    mode: AdbWebsocketMode,
  ): Promise<Result<AsyncWebsocket>> {
    const sock = await AsyncWebsocket.connect(wsUrl);
    if (sock === undefined) {
      return errResult(`Connection to ${wsUrl} failed`);
    }
    if (mode === 'WEBSOCKET_BRIDGE') {
      const transport = `host:transport:${deviceSerial}`;
      const status = await adbCmdAndWait(sock, transport, false);
      if (!status.ok) return status;
    }
    return okResult(sock);
  }

  override async createStream(svc: string): Promise<Result<WebSocketStream>> {
    const connRes = await AdbWebsocketDevice.connectToTransport(
      this.wsUrl,
      this.deviceSerial,
      this.mode,
    );
    if (!connRes.ok) return connRes;
    const sock = connRes.value;

    if (this.mode === 'WEBSOCKET_BRIDGE') {
      const status = await adbCmdAndWait(sock, svc, false);
      if (!status.ok) return status;
    } else if (this.mode === 'WEB_DEVICE_PROXY') {
      sock.send(
        JSON.stringify({
          header: {
            serialNumber: this.deviceSerial,
            command: svc,
          },
        }),
      );
    }
    const stream = new WebSocketStream(sock.release());
    this.streams.push(stream);
    return okResult(stream);
  }

  get connected(): boolean {
    return this.transportSock.connected;
  }

  override close(): void {
    this.transportSock.close();
    this.streams.forEach((s) => s.close());
    this.streams.splice(0);
  }
}
