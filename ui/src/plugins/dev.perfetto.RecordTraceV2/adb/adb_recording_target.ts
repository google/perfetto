// Copyright (C) 2026 The Android Open Source Project
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

import type protos from '../../../protos';
import {errResult, type Result} from '../../../base/result';
import {AsyncLazy} from '../../../base/async_lazy';
import type {ConsumerIpcTracingSession} from '../tracing_protocol/consumer_ipc_tracing_session';
import type {AdbDevice} from './adb_device';
import {
  createAdbTracingSession,
  getAdbTracingServiceState,
} from './adb_tracing_session';

// Behaviour shared by every adb-based RecordingTarget (WebUSB, WebSocket, Web
// Device Proxy). Subclasses only provide connectIfNeeded() and their identity
// (id / name / runPreflightChecks); everything that operates on the connected
// device lives here, so it isn't copy-pasted per transport.
export abstract class AdbRecordingTarget<T extends AdbDevice> {
  readonly kind = 'LIVE_RECORDING';
  readonly platform = 'ANDROID';
  protected adbDevice = new AsyncLazy<T>();

  // Connects to the device (or returns the cached connection).
  protected abstract connectIfNeeded(): Promise<Result<T>>;

  get connected(): boolean {
    return this.adbDevice.value?.connected ?? false;
  }

  async getServiceState(): Promise<Result<protos.ITracingServiceState>> {
    if (this.adbDevice.value === undefined) {
      return errResult('ADB transport disconnected');
    }
    return getAdbTracingServiceState(this.adbDevice.value);
  }

  async runShellCommand(cmd: string): Promise<Result<string>> {
    const dev = await this.connectIfNeeded();
    if (!dev.ok) return dev;
    return dev.value.shell(cmd);
  }

  async startTracing(
    traceConfig: protos.ITraceConfig,
  ): Promise<Result<ConsumerIpcTracingSession>> {
    const dev = await this.connectIfNeeded();
    if (!dev.ok) return dev;
    return createAdbTracingSession(dev.value, traceConfig);
  }

  disconnect(): void {
    this.adbDevice.value?.close();
    this.adbDevice.reset();
  }
}
