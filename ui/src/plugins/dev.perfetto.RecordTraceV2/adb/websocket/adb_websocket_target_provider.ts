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

import {errResult, okResult, Result} from '../../../../base/result';
import {exists} from '../../../../base/utils';
import {PreflightCheck} from '../../interfaces/connection_check';
import {AsyncWebsocket} from '../../websocket/async_websocket';
import {RecordingTargetProvider} from '../../interfaces/recording_target_provider';
import {AdbWebsocketTarget} from './adb_websocket_target';
import {adbCmdAndWait} from './adb_websocket_utils';
import {EvtSource} from '../../../../base/events';
import {websocketInstructions} from '../../websocket/websocket_utils';

export class AdbWebsocketTargetProvider implements RecordingTargetProvider {
  readonly id = 'adb_websocket';
  readonly name = 'ADB + WebSocket';
  readonly description =
    'This option uses the adbd server and can co-exist with other ' +
    'adb-based tools. Requires launching the websocket_bridge on the host.';
  readonly icon = 'lan';
  readonly supportedPlatforms = ['ANDROID'] as const;
  private readonly wsHost = '127.0.0.1:8037';
  readonly onTargetsChanged = new EvtSource<void>();
  private targets = new Map<string, AdbWebsocketTarget>();

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    yield {
      name: 'WebSocket connection',
      status: await (async (): Promise<Result<string>> => {
        using sock = await AsyncWebsocket.connect(this.wsUrl);
        return sock
          ? okResult('Connected')
          : errResult(
              `Failed to connect ${this.wsUrl}. ` +
                websocketInstructions('ANDROID'),
            );
      })(),
    };
  }

  async listTargets(): Promise<AdbWebsocketTarget[]> {
    await this.refreshTargets();
    return Array.from(this.targets.values());
  }

  private async refreshTargets() {
    const adbDevices = await this.listAdbdDevices();
    // Find and disconnected devices.
    for (const [serial, target] of this.targets.entries()) {
      if (!adbDevices.has(serial)) {
        target.disconnect();
        this.targets.delete(serial);
      }
    }
    // Find new devices.
    for (const [serial, model] of adbDevices.entries()) {
      if (this.targets.has(serial)) continue; // We already have a target.
      const newTarget = new AdbWebsocketTarget(this.wsUrl, serial, model);
      this.targets.set(serial, newTarget);
    }
  }

  // Returns a map of device serial -> product.
  private async listAdbdDevices(): Promise<Map<string, string>> {
    const devices = new Map<string, string>();
    using sock = await AsyncWebsocket.connect(this.wsUrl);
    if (!sock) return devices;
    const status = await adbCmdAndWait(sock, 'host:devices-l', true);
    if (!status.ok) return devices;
    for (const line of status.value.trimEnd().split('\n')) {
      if (line === '') continue;
      const m = line.match(/^([^\s]+)\s+.*model:([^ ]+)/);
      if (!exists(m)) {
        console.warn('Could not parse ADB device', line);
        continue;
      }
      const serial = m[1];
      const model = m[2];
      devices.set(serial, model);
    }
    return devices;
  }

  private get wsUrl(): string {
    return `ws://${this.wsHost}/adb`;
  }
}
