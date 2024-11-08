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

import {createEmptyState} from '../common/empty_state';
import {
  AdbRecordingTarget,
  LoadedConfig,
  RecordingState,
  RecordingTarget,
  getDefaultRecordingTargets,
  isAdbTarget,
} from '../common/state';
import {RECORDING_V2_FLAG} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {AdbOverWebUsb} from './adb';
import {isGetCategoriesResponse} from './chrome_proxy_record_controller';
import {RecordConfig, createEmptyRecordConfig} from './record_config_types';
import {RecordController} from './record_controller';

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';

// TODO(primiano): this class and RecordController should be merged. I'm keeping
// them separate for now to reduce scope of refactorings.
export class RecordingManager {
  private _state: RecordingState = createEmptyState();
  private recCtl: RecordController;

  // TODO(primiano): this singleton is temporary. RecordingManager shoudl be
  // injected in all the recording pages and the instance should be created and
  // owned by the recording plugin. But for now we don't have a plugin.
  private static _instance: RecordingManager | undefined = undefined;
  static get instance() {
    if (this._instance === undefined) {
      this._instance = new RecordingManager();
    }
    return this._instance;
  }

  constructor() {
    const extensionLocalChannel = new MessageChannel();
    this.recCtl = new RecordController(this, extensionLocalChannel.port1);
    this.setupExtentionPort(extensionLocalChannel);

    if (!RECORDING_V2_FLAG.get()) {
      this.updateAvailableAdbDevices();
      try {
        navigator.usb.addEventListener('connect', () =>
          this.updateAvailableAdbDevices(),
        );
        navigator.usb.addEventListener('disconnect', () =>
          this.updateAvailableAdbDevices(),
        );
      } catch (e) {
        console.error('WebUSB API not supported');
      }
    }
  }

  clearRecordConfig(): void {
    this._state.recordConfig = createEmptyRecordConfig();
    this._state.lastLoadedConfig = {type: 'NONE'};
    this.recCtl.refreshOnStateChange();
  }

  setRecordConfig(config: RecordConfig, configType?: LoadedConfig): void {
    this._state.recordConfig = config;
    this._state.lastLoadedConfig = configType || {type: 'NONE'};
    this.recCtl.refreshOnStateChange();
  }

  startRecording(): void {
    this._state.recordingInProgress = true;
    this._state.lastRecordingError = undefined;
    this._state.recordingCancelled = false;
    this.recCtl.refreshOnStateChange();
  }

  stopRecording(): void {
    this._state.recordingInProgress = false;
    this.recCtl.refreshOnStateChange();
  }

  cancelRecording(): void {
    this._state.recordingInProgress = false;
    this._state.recordingCancelled = true;
    this.recCtl.refreshOnStateChange();
  }

  setRecordingTarget(target: RecordingTarget): void {
    this._state.recordingTarget = target;
    this.recCtl.refreshOnStateChange();
  }

  setFetchChromeCategories(fetch: boolean): void {
    this._state.fetchChromeCategories = fetch;
    this.recCtl.refreshOnStateChange();
  }

  setAvailableAdbDevices(devices: AdbRecordingTarget[]): void {
    this._state.availableAdbDevices = devices;
    this.recCtl.refreshOnStateChange();
  }

  setLastRecordingError(error?: string): void {
    this._state.lastRecordingError = error;
    this._state.recordingStatus = undefined;
    this.recCtl.refreshOnStateChange();
  }

  setRecordingStatus(status?: string): void {
    this._state.recordingStatus = status;
    this._state.lastRecordingError = undefined;
    this.recCtl.refreshOnStateChange();
  }

  get state() {
    return this._state;
  }

  private setupExtentionPort(extensionLocalChannel: MessageChannel) {
    // We proxy messages between the extension and the controller because the
    // controller's worker can't access chrome.runtime.
    const extensionPort =
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      window.chrome && chrome.runtime
        ? chrome.runtime.connect(EXTENSION_ID)
        : undefined;

    this._state.extensionInstalled = extensionPort !== undefined;

    if (extensionPort) {
      // Send messages to keep-alive the extension port.
      const interval = setInterval(() => {
        extensionPort.postMessage({
          method: 'ExtensionVersion',
        });
      }, 25000);
      extensionPort.onDisconnect.addListener((_) => {
        this._state.extensionInstalled = false;
        clearInterval(interval);
        void chrome.runtime.lastError; // Needed to not receive an error log.
      });
      // This forwards the messages from the extension to the controller.
      extensionPort.onMessage.addListener(
        (message: object, _port: chrome.runtime.Port) => {
          if (isGetCategoriesResponse(message)) {
            this._state.chromeCategories = message.categories;
            raf.scheduleFullRedraw();
            return;
          }
          extensionLocalChannel.port2.postMessage(message);
        },
      );
    }

    // This forwards the messages from the controller to the extension
    extensionLocalChannel.port2.onmessage = ({data}) => {
      if (extensionPort) extensionPort.postMessage(data);
    };
  }

  async updateAvailableAdbDevices(preferredDeviceSerial?: string) {
    const devices = await new AdbOverWebUsb().getPairedDevices();

    let recordingTarget: AdbRecordingTarget | undefined = undefined;

    const availableAdbDevices: AdbRecordingTarget[] = [];
    devices.forEach((d) => {
      if (d.productName && d.serialNumber) {
        availableAdbDevices.push({
          name: d.productName,
          serial: d.serialNumber,
          os: 'S',
        });
        if (preferredDeviceSerial && preferredDeviceSerial === d.serialNumber) {
          recordingTarget = availableAdbDevices[availableAdbDevices.length - 1];
        }
      }
    });

    this.setAvailableAdbDevices(availableAdbDevices);
    this.selectAndroidDeviceIfAvailable(availableAdbDevices, recordingTarget);
    raf.scheduleFullRedraw();
    return availableAdbDevices;
  }

  private selectAndroidDeviceIfAvailable(
    availableAdbDevices: AdbRecordingTarget[],
    recordingTarget?: RecordingTarget,
  ) {
    if (!recordingTarget) {
      recordingTarget = this.state.recordingTarget;
    }
    const deviceConnected = isAdbTarget(recordingTarget);
    const connectedDeviceDisconnected =
      deviceConnected &&
      availableAdbDevices.find(
        (e) => e.serial === (recordingTarget as AdbRecordingTarget).serial,
      ) === undefined;

    if (availableAdbDevices.length) {
      // If there's an Android device available and the current selection isn't
      // one, select the Android device by default. If the current device isn't
      // available anymore, but another Android device is, select the other
      // Android device instead.
      if (!deviceConnected || connectedDeviceDisconnected) {
        recordingTarget = availableAdbDevices[0];
      }

      this.setRecordingTarget(recordingTarget);
      return;
    }

    // If the currently selected device was disconnected, reset the recording
    // target to the default one.
    if (connectedDeviceDisconnected) {
      this.setRecordingTarget(getDefaultRecordingTargets()[0]);
    }
  }
}
