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

import {assertExists} from '../../../base/logging';
import {AdbConnectionOverWebusb} from '../adb_connection_over_webusb';
import {AdbKeyManager} from '../auth/adb_key_manager';
import {
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';
import {TracedTracingSession} from '../traced_tracing_session';

export class AndroidWebusbTarget implements RecordingTargetV2 {
  private adbConnection: AdbConnectionOverWebusb;

  constructor(private device: USBDevice, keyManager: AdbKeyManager) {
    this.adbConnection = new AdbConnectionOverWebusb(device, keyManager);
  }

  getInfo(): TargetInfo {
    const name = assertExists(this.device.productName) + ' ' +
        assertExists(this.device.serialNumber) + ' WebUsb';
    // TODO(octaviant): fetch the OS from the adb connection
    return {targetType: 'ANDROID', androidApiLevel: 31, name};
  }

  // This is called when a usb USBConnectionEvent of type 'disconnect' event is
  // emitted. This event is emitted when the USB connection is lost(example:
  // when the user unplugged the connecting cable).
  async disconnect(disconnectMessage?: string): Promise<void> {
    await this.adbConnection.disconnect(disconnectMessage);
  }

  async createTracingSession(tracingSessionListener: TracingSessionListener):
      Promise<TracingSession> {
    this.adbConnection.onStatus = tracingSessionListener.onStatus;
    this.adbConnection.onDisconnect = tracingSessionListener.onDisconnect;
    const adbStream =
        await this.adbConnection.connectSocket('/dev/socket/traced_consumer');

    const tracingSession =
        new TracedTracingSession(adbStream, tracingSessionListener);
    await tracingSession.initConnection();
    return tracingSession;
  }
}
