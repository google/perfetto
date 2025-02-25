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

import {errResult, okResult, Result} from '../../../../base/result';
import {PreflightCheck} from '../../interfaces/connection_check';
import {AsyncWebsocket} from '../../websocket/async_websocket';
import {RecordingTargetProvider} from '../../interfaces/recording_target_provider';
import {EvtSource} from '../../../../base/events';
import {WebDeviceProxyTarget as WdpDeviceProxyTarget} from './wdp_target';
import {showPopupWindow} from '../../../../base/popup_window';
import {
  WdpTrackDevicesResponse,
  WdpDevice,
  WDP_TRACK_DEVICES_SCHEMA,
} from './wdp_schema';
import {disposeWebsocket} from '../../websocket/websocket_utils';
import {AsyncLazy} from '../../../../base/async_lazy';

const WDP_URL = 'https://tools.google.com/dlpage/android_web_device_proxy';

// WDP = Web Device Proxy (go/external-web-device-proxy). This works very
// similarly to our websocket_bridge, with few differences in the handshake.
export class WebDeviceProxyTargetProvider implements RecordingTargetProvider {
  readonly id = 'adb_wdp';
  readonly name = 'ADB + WebDeviceProxy';
  readonly description =
    'This option uses the adbd server and can co-exist ' +
    'with other adb-based tools. Requires ' +
    WDP_URL +
    '\nGoogle employees: see go/web-device-proxy';
  readonly icon = 'corporate_fare';
  readonly supportedPlatforms = ['ANDROID'] as const;
  readonly onTargetsChanged = new EvtSource<void>();
  private targets = new Map<string, WdpDeviceProxyTarget>();

  // Wraps the websocket listening for device changes on /track-devices.json.
  private trackDevicesConn = new AsyncLazy<TrackDevicesConnection>();

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    const trackDevConn = await this.connectIfNeeded();

    yield {
      name: 'Web Device Proxy',
      status: trackDevConn.ok
        ? okResult(`${trackDevConn.value.wdpVersion}`)
        : trackDevConn,
    };
    if (!trackDevConn.ok) return;

    yield {
      name: 'List devices',
      status: okResult(`${this.targets.size} devices found`),
    };
  }

  async listTargets(): Promise<WdpDeviceProxyTarget[]> {
    await this.connectIfNeeded();
    return Array.from(this.targets.values());
  }

  // Returns the version of WDP (e.g. "androidbuild_web_device_proxy_linux_1.2")
  // if the connection succeeds (and populates this.targets). Otherwise returns
  // an actionable error.
  private connectIfNeeded(): Promise<Result<TrackDevicesConnection>> {
    return this.trackDevicesConn.getOrCreate(async () => {
      const wsUrl = 'ws://127.0.0.1:9167/track-devices-json';
      let aws: AsyncWebsocket | undefined;
      for (let attempt = 0; attempt < 2; attempt++) {
        aws = await AsyncWebsocket.connect(wsUrl);
        if (aws === undefined) {
          return errResult(
            `Failed to connect to ${wsUrl}. WDP doesn't seem to be running.` +
              `Follow the instructions on go/web-device-proxy`,
          );
        }
        const respStr = await aws.waitForString();
        const respJson = JSON.parse(respStr);
        const respSchema = WDP_TRACK_DEVICES_SCHEMA.safeParse(respJson);
        if (!respSchema.success) {
          return errResult(`Failed to parse ${respStr}: ${respSchema.error}`);
        }
        const resp = respSchema.data;

        if (
          resp.error?.type === 'ORIGIN_NOT_ALLOWLISTED' &&
          resp.error.approveUrl !== undefined
        ) {
          // This happens the very first time we use WDP. It just tells us we
          // need to show a popup to let the user allow us to talk to WDP.
          const popup = await showPopupWindow({url: resp.error.approveUrl});
          if (popup === false) {
            return errResult('You need to enable popups and try again');
          }
          continue; // Do another attempt now that the user allowed the origin.
        } else if (resp.error !== undefined) {
          return errResult(resp.error.message ?? 'Unknown WDP Error');
        }

        // No error, we got a valid connection with some deviceInfo.
        // We want to parse the first response we got and also keep listening
        // for updates that will come in future.
        const ws = aws.release();
        ws.onclose = () => this.destroyTrackDevicesConnection();
        ws.onerror = () => this.destroyTrackDevicesConnection();
        ws.onmessage = (e: MessageEvent<string>) => {
          const resp = WDP_TRACK_DEVICES_SCHEMA.safeParse(JSON.parse(e.data));
          if (resp.success) {
            this.onTrackDevicesResponse(resp.data);
          } else {
            console.error(`Invalid WDP response ${e.data} : ${resp.error}`);
          }
        };
        const connResult = {
          wdpVersion: resp.version ?? 'N/A',
          ws,
        };
        this.onTrackDevicesResponse(resp);
        return okResult(connResult);
      } // for(attempt)
      return errResult(
        'Failed all attempts to authenticate on WDP.' +
          'You must click allow on the popup to use WDP.',
      );
    });
  }

  private destroyTrackDevicesConnection() {
    const ws = this.trackDevicesConn.value?.ws;
    this.trackDevicesConn.reset();
    ws && disposeWebsocket(ws);
  }

  // This function is called every time /track-devices-json sends a new message,
  // typically every time there is a device {dis,}connection.
  private onTrackDevicesResponse(resp: WdpTrackDevicesResponse) {
    if (resp.error !== undefined) {
      this.destroyTrackDevicesConnection();
      return;
    }

    // Build a map (serial -> device) from the response array.
    const curDevs = new Map<string, WdpDevice>(
      (resp.device ?? []).map((d) => [d.serialNumber, d]),
    );

    // Identify and disconnected devices that are no longer connected.
    for (const [serial, target] of this.targets.entries()) {
      if (!curDevs.has(serial)) {
        target.disconnect();
        this.targets.delete(serial);
      }
    }

    // Identify new devices.
    for (const [serial, devJson] of curDevs.entries()) {
      const existingDevice = this.targets.get(serial);
      if (existingDevice !== undefined) {
        // We saw the device already and have created a WdpDeviceProxyTarget.
        // The only thing we need to do is to update its descriptor, as the
        // device might transition between UNAUTHORIZED <> OFFLINE <> DEVICE.
        existingDevice.updateWdpState(devJson);
      } else {
        const wsUrl = 'ws://127.0.0.1:9167/adb-json';
        const newTarget = new WdpDeviceProxyTarget(wsUrl, devJson);
        this.targets.set(serial, newTarget);
      }
    }

    this.onTargetsChanged.notify();
  }
}

interface TrackDevicesConnection {
  wdpVersion: string;
  ws: WebSocket;
}
