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
import {RecordingTarget} from '../../interfaces/recording_target';
import {PreflightCheck} from '../../interfaces/connection_check';
import {AdbKeyManager} from './adb_key_manager';
import {
  createAdbTracingSession,
  getAdbTracingServiceState,
} from '../adb_tracing_session';
import {AdbWebusbDevice} from './adb_webusb_device';
import {AdbUsbInterface, usbDeviceToStr} from './adb_webusb_utils';
import {errResult, okResult, Result} from '../../../../base/result';
import {checkAndroidTarget} from '../adb_platform_checks';
import {ConsumerIpcTracingSession} from '../../tracing_protocol/consumer_ipc_tracing_session';
import {AsyncLazy} from '../../../../base/async_lazy';

export class AdbWebusbTarget implements RecordingTarget {
  readonly kind = 'LIVE_RECORDING';
  readonly platform = 'ANDROID';
  readonly transportType = 'WebUSB';
  private adbDevice = new AsyncLazy<AdbWebusbDevice>();

  constructor(
    private usbiface: AdbUsbInterface,
    private adbKeyMgr: AdbKeyManager,
  ) {}

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    const status = await this.connectIfNeeded();

    yield {
      name: 'WebUSB connection',
      status: await (async (): Promise<Result<string>> => {
        if (!status.ok) return status;
        return okResult('connected');
      })(),
    };

    if (this.adbDevice.value === undefined) return;
    yield* checkAndroidTarget(this.adbDevice.value);
  }

  async connectIfNeeded(): Promise<Result<AdbWebusbDevice>> {
    return this.adbDevice.getOrCreate(() =>
      AdbWebusbDevice.connect(this.usbiface.dev, this.adbKeyMgr),
    );
  }

  get connected(): boolean {
    return this.adbDevice.value?.connected ?? false;
  }

  get id(): string {
    return usbDeviceToStr(this.usbiface.dev);
  }

  get name(): string {
    const dev = this.usbiface.dev;
    return `${dev.productName} [${dev.serialNumber}]`;
  }

  async getServiceState(): Promise<Result<protos.ITracingServiceState>> {
    if (this.adbDevice.value === undefined) {
      return errResult('WebUSB transport disconnected');
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

  disconnect(): void {
    this.adbDevice.value?.close();
    this.adbDevice.reset();
  }
}
