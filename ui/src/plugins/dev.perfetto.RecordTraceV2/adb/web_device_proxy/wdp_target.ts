// Copyright (C) 2025 The Android Open Source Project
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
import {AdbWebsocketDevice} from '../websocket/adb_websocket_device';
import {AsyncLazy} from '../../../../base/async_lazy';
import {WdpDevice} from './wdp_schema';
import {showPopupWindow} from '../../../../base/popup_window';
import {defer} from '../../../../base/deferred';

export class WebDeviceProxyTarget implements RecordingTarget {
  readonly kind = 'LIVE_RECORDING';
  readonly platform = 'ANDROID';

  private adbDevice = new AsyncLazy<AdbWebsocketDevice>();
  readonly id: string;

  constructor(
    private wsUrl: string,
    private devJson: WdpDevice,
  ) {
    this.id = this.devJson.serialNumber;
    this.updateWdpState(devJson);
  }

  // This is called by WdpTragetProvider every time a state change is received.
  // The challenge here is that we have two websockets: a global one to list
  // devices via /track-devices-json owned by WdpTargetProvider; a per-device
  // one to /adb-json owned by us. Unfortunately the status updates are sent via
  // the former, so we need WdpTargetProvider to inform us about state changes.
  updateWdpState(devJson: WdpDevice) {
    this.devJson = devJson;
  }

  // Returns a successful Result if the device is ready to trace, or an error
  // Result if the device is in a state we don't recognize.
  private deviceReady(): Result<string> {
    // The return string is the same both in case of success or failure.
    const status =
      `proxyStatus=${this.devJson.proxyStatus} ` +
      ` adbStatus=${this.devJson.adbStatus}`;

    if (
      this.devJson.proxyStatus === 'ADB' &&
      this.devJson.adbStatus === 'DEVICE'
    ) {
      return okResult(status);
    }
    return errResult(status);
  }

  get name(): string {
    if (this.devJson.proxyStatus === 'ADB') {
      if (this.devJson.adbStatus === 'DEVICE') {
        return `${this.devJson.adbProps?.model ?? '?'} [${this.id}]`;
      }
      return `${this.devJson.adbStatus} [${this.id}]`;
    }
    return `${this.devJson.proxyStatus} [${this.id}]`;
  }

  get connected(): boolean {
    return this.adbDevice.value?.connected ?? false;
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    await this.connectIfNeeded();

    yield {
      name: 'Web Device Proxy',
      status: this.deviceReady(),
    };
    if (this.adbDevice.value === undefined) return;
    yield* checkAndroidTarget(this.adbDevice.value);
  }

  private async connectIfNeeded(): Promise<Result<AdbWebsocketDevice>> {
    return this.adbDevice.getOrCreate(async () => {
      for (let attempt = 0; attempt < 2; attempt++) {
        if (this.devJson.proxyStatus === 'PROXY_UNAUTHORIZED') {
          const res = await showPopupWindow({url: this.devJson.approveUrl});
          if (!res) {
            return errResult('Enable popups and try again');
          }
          // At this point either the device transitions into the authorized
          // state or some error state. Give some time for the WDP to reach the
          // final state, whatever it is. If we remove this delay we'll see a
          // device in a 'AUTHORIZING' state and won't be able to progress.
          // If this time is not enough, the user will have to manually press
          // on the refresh button to re-run the pre-flight checks and get the
          // most up-to-date state.
          const wait = defer<void>();
          setTimeout(() => wait.resolve(), 250);
          await wait;
        }
        const ready = this.deviceReady();
        if (!ready.ok) return ready;
        return AdbWebsocketDevice.connect(
          this.wsUrl,
          this.id,
          'WEB_DEVICE_PROXY',
        );
      } // for(attempt)
      return errResult(
        'WDP authorization failed. Follow the WDP popup, ' +
          'authorize access and try again',
      );
    });
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
