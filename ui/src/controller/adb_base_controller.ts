// Copyright (C) 2019 The Android Open Source Project
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

import {exists} from '../base/utils';
import {RecordingState, RecordingTarget, isAdbTarget} from '../common/state';
import {
  extractDurationFromTraceConfig,
  extractTraceConfig,
} from '../core/trace_config_utils';
import {Adb} from './adb_interfaces';
import {ReadBuffersResponse} from './consumer_port_types';
import {Consumer, RpcConsumerPort} from './record_controller_interfaces';

export enum AdbConnectionState {
  READY_TO_CONNECT,
  AUTH_IN_PROGRESS,
  CONNECTED,
  CLOSED,
}

interface Command {
  method: string;
  params: Uint8Array;
}

export abstract class AdbBaseConsumerPort extends RpcConsumerPort {
  // Contains the commands sent while the authentication is in progress. They
  // will all be executed afterwards. If the device disconnects, they are
  // removed.
  private commandQueue: Command[] = [];

  protected adb: Adb;
  protected state = AdbConnectionState.READY_TO_CONNECT;
  protected device?: USBDevice;
  protected recState: RecordingState;

  protected constructor(
    adb: Adb,
    consumer: Consumer,
    recState: RecordingState,
  ) {
    super(consumer);
    this.adb = adb;
    this.recState = recState;
  }

  async handleCommand(method: string, params: Uint8Array) {
    try {
      if (method === 'FreeBuffers') {
        // When we finish tracing, we disconnect the adb device interface.
        // Otherwise, we will keep holding the device interface and won't allow
        // adb to access it. https://wicg.github.io/webusb/#abusing-a-device
        // "Lastly, since USB devices are unable to distinguish requests from
        // multiple sources, operating systems only allow a USB interface to
        // have a single owning user-space or kernel-space driver."
        this.state = AdbConnectionState.CLOSED;
        await this.adb.disconnect();
      } else if (method === 'EnableTracing') {
        this.state = AdbConnectionState.READY_TO_CONNECT;
      }

      if (this.state === AdbConnectionState.CLOSED) return;

      this.commandQueue.push({method, params});

      if (
        this.state === AdbConnectionState.READY_TO_CONNECT ||
        this.deviceDisconnected()
      ) {
        this.state = AdbConnectionState.AUTH_IN_PROGRESS;
        this.device = await this.findDevice(this.recState.recordingTarget);
        if (!this.device) {
          this.state = AdbConnectionState.READY_TO_CONNECT;
          const target = this.recState.recordingTarget;
          throw Error(
            `Device with serial ${
              isAdbTarget(target) ? target.serial : 'n/a'
            } not found.`,
          );
        }

        this.sendStatus(`Please allow USB debugging on device.
          If you press cancel, reload the page.`);

        await this.adb.connect(this.device);

        // During the authentication the device may have been disconnected.
        if (!this.recState.recordingInProgress || this.deviceDisconnected()) {
          throw Error('Recording not in progress after adb authorization.');
        }

        this.state = AdbConnectionState.CONNECTED;
        this.sendStatus('Device connected.');
      }

      if (this.state === AdbConnectionState.AUTH_IN_PROGRESS) return;

      console.assert(this.state === AdbConnectionState.CONNECTED);

      for (const cmd of this.commandQueue) this.invoke(cmd.method, cmd.params);

      this.commandQueue = [];
    } catch (e) {
      this.commandQueue = [];
      this.state = AdbConnectionState.READY_TO_CONNECT;
      this.sendErrorMessage(e.message);
    }
  }

  private deviceDisconnected() {
    return !this.device || !this.device.opened;
  }

  setDurationStatus(enableTracingProto: Uint8Array) {
    const traceConfigProto = extractTraceConfig(enableTracingProto);
    if (!traceConfigProto) return;
    const duration = extractDurationFromTraceConfig(traceConfigProto);
    this.sendStatus(
      `Recording in progress${
        exists(duration) ? ' for ' + duration.toString() + ' ms' : ''
      }...`,
    );
  }

  abstract invoke(method: string, argsProto: Uint8Array): void;

  generateChunkReadResponse(
    data: Uint8Array,
    last = false,
  ): ReadBuffersResponse {
    return {
      type: 'ReadBuffersResponse',
      slices: [{data, lastSliceForPacket: last}],
    };
  }

  async findDevice(
    connectedDevice: RecordingTarget,
  ): Promise<USBDevice | undefined> {
    if (!('usb' in navigator)) return undefined;
    if (!isAdbTarget(connectedDevice)) return undefined;
    const devices = await navigator.usb.getDevices();
    return devices.find((d) => d.serialNumber === connectedDevice.serial);
  }
}
