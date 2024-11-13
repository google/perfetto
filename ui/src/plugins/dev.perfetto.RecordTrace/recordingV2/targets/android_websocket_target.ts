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

import {AdbConnectionOverWebsocket} from '../adb_connection_over_websocket';
import {OnTargetChangeCallback, TargetInfo} from '../recording_interfaces_v2';
import {AndroidTarget} from './android_target';

export class AndroidWebsocketTarget extends AndroidTarget {
  constructor(
    private serialNumber: string,
    websocketUrl: string,
    onTargetChange: OnTargetChangeCallback,
  ) {
    super(
      new AdbConnectionOverWebsocket(serialNumber, websocketUrl),
      onTargetChange,
    );
  }

  getInfo(): TargetInfo {
    return {
      targetType: 'ANDROID',
      // 'androidApiLevel' will be populated after ADB authorization.
      androidApiLevel: this.androidApiLevel,
      dataSources: this.dataSources || [],
      name: this.serialNumber + ' WebSocket',
    };
  }
}
