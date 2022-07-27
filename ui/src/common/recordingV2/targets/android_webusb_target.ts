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

import {fetchWithTimeout} from '../../../base/http_utils';
import {assertExists} from '../../../base/logging';
import {VERSION} from '../../../gen/perfetto_version';
import {AdbConnectionOverWebusb} from '../adb_connection_over_webusb';
import {
  TRACEBOX_DEVICE_PATH,
  TRACEBOX_FETCH_TIMEOUT,
} from '../adb_targets_utils';
import {AdbKeyManager} from '../auth/adb_key_manager';
import {
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';
import {
  AndroidWebusbTargetFactory,
} from '../target_factories/android_webusb_target_factory';
import {TracedTracingSession} from '../traced_tracing_session';

export class AndroidWebusbTarget implements RecordingTargetV2 {
  private adbConnection: AdbConnectionOverWebusb;
  private androidApiLevel?: number;

  constructor(
      private factory: AndroidWebusbTargetFactory, private device: USBDevice,
      keyManager: AdbKeyManager) {
    this.adbConnection = new AdbConnectionOverWebusb(device, keyManager);
  }

  getInfo(): TargetInfo {
    const name = assertExists(this.device.productName) + ' ' +
        assertExists(this.device.serialNumber) + ' WebUsb';
    return {
      targetType: 'ANDROID',
      // The method 'fetchInfo' will populate this after ADB authorization.
      androidApiLevel: this.androidApiLevel,
      dataSources: [],
      name,
    };
  }

  // This is called when a usb USBConnectionEvent of type 'disconnect' event is
  // emitted. This event is emitted when the USB connection is lost(example:
  // when the user unplugged the connecting cable).
  async disconnect(disconnectMessage?: string): Promise<void> {
    await this.adbConnection.disconnect(disconnectMessage);
  }

  // Starts a tracing session in order to fetch information such as apiLevel
  // and dataSources from the device. Then, it cancels the session.
  async fetchTargetInfo(tracingSessionListener: TracingSessionListener):
      Promise<void> {
    const tracingSession =
        await this.createTracingSession(tracingSessionListener);
    tracingSession.cancel();
  }

  async createTracingSession(tracingSessionListener: TracingSessionListener):
      Promise<TracingSession> {
    this.adbConnection.onStatus = tracingSessionListener.onStatus;
    this.adbConnection.onDisconnect = tracingSessionListener.onDisconnect;
    const adbStream =
        await this.adbConnection.connectSocket('/dev/socket/traced_consumer');

    if (!this.androidApiLevel) {
      const version = await this.adbConnection.shellAndGetOutput(
          'getprop ro.build.version.sdk');
      this.androidApiLevel = Number(version);
      if (this.factory.onTargetChange) {
        this.factory.onTargetChange();
      }

      // For older OS versions we push the tracebox binary.
      if (this.androidApiLevel < 29) {
        await this.pushTracebox();
      }
    }

    const tracingSession =
        new TracedTracingSession(adbStream, tracingSessionListener);
    await tracingSession.initConnection();
    return tracingSession;
  }

  async pushTracebox() {
    const arch = await this.fetchArchitecture();
    const shortVersion = VERSION.split('-')[0];
    const traceboxBin =
        await (
            await fetchWithTimeout(
                `https://commondatastorage.googleapis.com/perfetto-luci-artifacts/${
                    shortVersion}/${arch}/tracebox`,
                {method: 'get'},
                TRACEBOX_FETCH_TIMEOUT))
            .arrayBuffer();
    await this.adbConnection.push(
        new Uint8Array(traceboxBin), TRACEBOX_DEVICE_PATH);

    // We explicitly set the tracebox permissions because adb does not reliably
    // set permissions when uploading the binary.
    await this.adbConnection.shellAndGetOutput(
        `chmod 755 ${TRACEBOX_DEVICE_PATH}`);
  }

  async fetchArchitecture() {
    const abiList = await this.adbConnection.shellAndGetOutput(
        'getprop ro.vendor.product.cpu.abilist');
    // If multiple ABIs are allowed, the 64bit ones should have higher priority.
    if (abiList.includes('arm64-v8a')) {
      return 'android-arm64';
    } else if (abiList.includes('x86')) {
      return 'android-x86';
    } else if (abiList.includes('armeabi-v7a') || abiList.includes('armeabi')) {
      return 'android-arm';
    } else if (abiList.includes('x86_64')) {
      return 'android-x64';
    }
    // Most devices have arm64 architectures, so we should return this if
    // nothing else is found.
    return 'android-arm64';
  }

  canConnectWithoutContention(): Promise<boolean> {
    return this.adbConnection.canConnectWithoutContention();
  }
}
