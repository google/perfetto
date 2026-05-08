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

import protos from '../../../../protos';
import {errResult, okResult, Result} from '../../../../base/result';
import {PreflightCheck} from '../../interfaces/connection_check';
import {RecordingTarget} from '../../interfaces/recording_target';
import {ConsumerIpcTracingSession} from '../../tracing_protocol/consumer_ipc_tracing_session';
import {checkAndroidTarget} from '../adb_platform_checks';
import {
  createAdbTracingSession,
  getAdbTracingServiceState,
} from '../adb_tracing_session';
import {AdbWebsocketDevice} from './adb_websocket_device';
import {AsyncLazy} from '../../../../base/async_lazy';

export class AdbWebsocketTarget implements RecordingTarget {
  readonly kind = 'LIVE_RECORDING';
  readonly platform = 'ANDROID';
  readonly transportType = 'WebSocket';

  private adbDevice = new AsyncLazy<AdbWebsocketDevice>();

  constructor(
    private wsUrl: string,
    private serial: string,
    private model: string,
  ) {}

  get id(): string {
    return this.serial;
  }

  get name(): string {
    return `${this.model} [${this.serial}]`;
  }

  get connected(): boolean {
    return this.adbDevice.value?.connected ?? false;
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    yield {
      name: 'WebSocket connection',
      status: await (async (): Promise<Result<string>> => {
        const status = await this.connectIfNeeded();
        if (!status.ok) return status;
        return okResult('connected');
      })(),
    };
    if (this.adbDevice.value === undefined) return;
    yield* checkAndroidTarget(this.adbDevice.value);
  }

  private async connectIfNeeded(): Promise<Result<AdbWebsocketDevice>> {
    return this.adbDevice.getOrCreate(() =>
      AdbWebsocketDevice.connect(this.wsUrl, this.serial, 'WEBSOCKET_BRIDGE'),
    );
  }

  disconnect(): void {
    // There isn't much to do in this case. If the device is disconnected,
    // the per-stream sockets will be naturally closed by adb. In turn,
    // websocket_bridge will propagate that as a closure of the per-stream
    // WebSockets.
    this.adbDevice.value?.close();
    this.adbDevice.reset();
  }

  async getServiceState(): Promise<Result<protos.ITracingServiceState>> {
    if (this.adbDevice.value === undefined) {
      return errResult('WebSocket transport disconnected');
    }
    return getAdbTracingServiceState(this.adbDevice.value);
  }

  async startTracing(
    traceConfig: protos.ITraceConfig,
  ): Promise<Result<ConsumerIpcTracingSession>> {
    const adbDeviceStatus = await this.connectIfNeeded();
    if (!adbDeviceStatus.ok) return adbDeviceStatus;
    return await createAdbTracingSession(adbDeviceStatus.value, traceConfig);
  }
}
