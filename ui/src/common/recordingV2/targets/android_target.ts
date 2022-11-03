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
import {VERSION} from '../../../gen/perfetto_version';
import {AdbConnectionImpl} from '../adb_connection_impl';
import {
  DataSource,
  OnTargetChangeCallback,
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';
import {
  CUSTOM_TRACED_CONSUMER_SOCKET_PATH,
  DEFAULT_TRACED_CONSUMER_SOCKET_PATH,
  TRACEBOX_DEVICE_PATH,
  TRACEBOX_FETCH_TIMEOUT,
} from '../recording_utils';
import {TracedTracingSession} from '../traced_tracing_session';

export abstract class AndroidTarget implements RecordingTargetV2 {
  private consumerSocketPath = DEFAULT_TRACED_CONSUMER_SOCKET_PATH;
  protected androidApiLevel?: number;
  protected dataSources?: DataSource[];

  protected constructor(
      private adbConnection: AdbConnectionImpl,
      private onTargetChange: OnTargetChangeCallback) {}

  abstract getInfo(): TargetInfo;

  // This is called when a usb USBConnectionEvent of type 'disconnect' event is
  // emitted. This event is emitted when the USB connection is lost (example:
  // when the user unplugged the connecting cable).
  async disconnect(disconnectMessage?: string): Promise<void> {
    await this.adbConnection.disconnect(disconnectMessage);
  }

  // Starts a tracing session in order to fetch information such as apiLevel
  // and dataSources from the device. Then, it cancels the session.
  async fetchTargetInfo(listener: TracingSessionListener): Promise<void> {
    const tracingSession = await this.createTracingSession(listener);
    tracingSession.cancel();
  }

  // We do not support long tracing on Android.
  canCreateTracingSession(recordingMode: string): boolean {
    return recordingMode !== 'LONG_TRACE';
  }

  async createTracingSession(tracingSessionListener: TracingSessionListener):
      Promise<TracingSession> {
    this.adbConnection.onStatus = tracingSessionListener.onStatus;
    this.adbConnection.onDisconnect = tracingSessionListener.onDisconnect;

    if (!this.androidApiLevel) {
      // 1. Fetch the API version from the device.
      const version = await this.adbConnection.shellAndGetOutput(
          'getprop ro.build.version.sdk');
      this.androidApiLevel = Number(version);

      this.onTargetChange();

      // 2. For older OS versions we push the tracebox binary.
      if (this.androidApiLevel < 29) {
        await this.pushTracebox();
        this.consumerSocketPath = CUSTOM_TRACED_CONSUMER_SOCKET_PATH;

        await this.adbConnection.shellAndWaitCompletion(
            this.composeTraceboxCommand('traced'));
        await this.adbConnection.shellAndWaitCompletion(
            this.composeTraceboxCommand('traced_probes'));
      }
    }

    const adbStream =
        await this.adbConnection.connectSocket(this.consumerSocketPath);

    // 3. Start a tracing session.
    const tracingSession =
        new TracedTracingSession(adbStream, tracingSessionListener);
    await tracingSession.initConnection();

    if (!this.dataSources) {
      // 4. Fetch dataSources from QueryServiceState.
      this.dataSources = await tracingSession.queryServiceState();

      this.onTargetChange();
    }
    return tracingSession;
  }

  async pushTracebox() {
    const arch = await this.fetchArchitecture();
    const shortVersion = VERSION.split('-')[0];
    const requestUrl =
        `https://commondatastorage.googleapis.com/perfetto-luci-artifacts/${
            shortVersion}/${arch}/tracebox`;
    const fetchResponse = await fetchWithTimeout(
        requestUrl, {method: 'get'}, TRACEBOX_FETCH_TIMEOUT);
    const traceboxBin = await fetchResponse.arrayBuffer();
    await this.adbConnection.push(
        new Uint8Array(traceboxBin), TRACEBOX_DEVICE_PATH);

    // We explicitly set the tracebox permissions because adb does not reliably
    // set permissions when uploading the binary.
    await this.adbConnection.shellAndWaitCompletion(
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

  composeTraceboxCommand(applet: string) {
    // 1. Set the consumer socket.
    return 'PERFETTO_CONSUMER_SOCK_NAME=@traced_consumer ' +
        // 2. Set the producer socket.
        'PERFETTO_PRODUCER_SOCK_NAME=@traced_producer ' +
        // 3. Start the applet in the background.
        `/data/local/tmp/tracebox ${applet} --background`;
  }
}
