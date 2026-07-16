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

import type {PreflightCheck} from '../../interfaces/connection_check';
import type {RecordingTarget} from '../../interfaces/recording_target';
import {okResult, type Result} from '../../../../base/result';
import {checkAndroidTarget} from '../adb_platform_checks';
import {AdbWebsocketDevice} from './adb_websocket_device';
import {AdbRecordingTarget} from '../adb_recording_target';

export class AdbWebsocketTarget
  extends AdbRecordingTarget<AdbWebsocketDevice>
  implements RecordingTarget
{
  readonly transportType = 'WebSocket';

  constructor(
    private wsUrl: string,
    private serial: string,
    private model: string,
  ) {
    super();
  }

  get id(): string {
    return this.serial;
  }

  get name(): string {
    return `${this.model} [${this.serial}]`;
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

  protected connectIfNeeded(): Promise<Result<AdbWebsocketDevice>> {
    return this.adbDevice.getOrCreate(() =>
      AdbWebsocketDevice.connect(this.wsUrl, this.serial, 'WEBSOCKET_BRIDGE'),
    );
  }
}
