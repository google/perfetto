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
import {getErrorMessage} from '../../errors';
import {RECORDING_V2_FLAG} from '../../feature_flags';
import {AdbKeyManager} from '../auth/adb_key_manager';
import {RecordingError} from '../recording_error_handling';
import {
  OnTargetChangeCallback,
  RecordingTargetV2,
  TargetFactory,
} from '../recording_interfaces_v2';
import {ADB_DEVICE_FILTER, findInterfaceAndEndpoint} from '../recording_utils';
import {targetFactoryRegistry} from '../target_factory_registry';
import {AndroidWebusbTarget} from '../targets/android_webusb_target';

export const ANDROID_WEBUSB_TARGET_FACTORY = 'AndroidWebusbTargetFactory';
const SERIAL_NUMBER_ISSUE = 'an invalid serial number';
const ADB_INTERFACE_ISSUE = 'an incompatible adb interface';

interface DeviceValidity {
  isValid: boolean;
  issues: string[];
}

function createDeviceErrorMessage(device: USBDevice, issue: string): string {
  const productName = device.productName;
  return `USB device${productName ? ' ' + productName : ''} has ${issue}`;
}

export class AndroidWebusbTargetFactory implements TargetFactory {
  readonly kind = ANDROID_WEBUSB_TARGET_FACTORY;
  onTargetChange: OnTargetChangeCallback = () => {};
  private recordingProblems: string[] = [];
  private targets: Map<string, AndroidWebusbTarget> =
      new Map<string, AndroidWebusbTarget>();
  // AdbKeyManager should only be instantiated once, so we can use the same key
  // for all devices.
  private keyManager: AdbKeyManager = new AdbKeyManager();

  constructor(private usb: USB) {
    this.init();
  }

  getName() {
    return 'Android WebUsb';
  }

  listTargets(): RecordingTargetV2[] {
    return Array.from(this.targets.values());
  }

  listRecordingProblems(): string[] {
    return this.recordingProblems;
  }

  async connectNewTarget(): Promise<RecordingTargetV2> {
    let device: USBDevice;
    try {
      device = await this.usb.requestDevice({filters: [ADB_DEVICE_FILTER]});
    } catch (e) {
      throw new RecordingError(getErrorMessage(e));
    }

    const deviceValid = this.checkDeviceValidity(device);
    if (!deviceValid.isValid) {
      throw new RecordingError(deviceValid.issues.join('\n'));
    }

    const androidTarget =
        new AndroidWebusbTarget(device, this.keyManager, this.onTargetChange);
    this.targets.set(assertExists(device.serialNumber), androidTarget);
    return androidTarget;
  }

  setOnTargetChange(onTargetChange: OnTargetChangeCallback) {
    this.onTargetChange = onTargetChange;
  }

  private async init() {
    let devices: USBDevice[] = [];
    try {
      devices = await this.usb.getDevices();
    } catch (_) {
      return;  // WebUSB not available or disallowed in iframe.
    }

    for (const device of devices) {
      if (this.checkDeviceValidity(device).isValid) {
        this.targets.set(
            assertExists(device.serialNumber),
            new AndroidWebusbTarget(
                device, this.keyManager, this.onTargetChange));
      }
    }

    this.usb.addEventListener('connect', (ev: USBConnectionEvent) => {
      if (this.checkDeviceValidity(ev.device).isValid) {
        this.targets.set(
            assertExists(ev.device.serialNumber),
            new AndroidWebusbTarget(
                ev.device, this.keyManager, this.onTargetChange));
        this.onTargetChange();
      }
    });

    this.usb.addEventListener('disconnect', async (ev: USBConnectionEvent) => {
      // We don't check device validity when disconnecting because if the device
      // is invalid we would not have connected in the first place.
      const serialNumber = assertExists(ev.device.serialNumber);
      await assertExists(this.targets.get(serialNumber))
          .disconnect(`Device with serial ${serialNumber} was disconnected.`);
      this.targets.delete(serialNumber);
      this.onTargetChange();
    });
  }

  private checkDeviceValidity(device: USBDevice): DeviceValidity {
    const deviceValidity: DeviceValidity = {isValid: true, issues: []};
    if (!device.serialNumber) {
      deviceValidity.issues.push(
          createDeviceErrorMessage(device, SERIAL_NUMBER_ISSUE));
      deviceValidity.isValid = false;
    }
    if (!findInterfaceAndEndpoint(device)) {
      deviceValidity.issues.push(
          createDeviceErrorMessage(device, ADB_INTERFACE_ISSUE));
      deviceValidity.isValid = false;
    }
    this.recordingProblems.push(...deviceValidity.issues);
    return deviceValidity;
  }
}

// We only want to instantiate this class if:
// 1. The browser implements the USB functionality.
// 2. Recording V2 is enabled.
if (navigator.usb && RECORDING_V2_FLAG.get()) {
  targetFactoryRegistry.register(new AndroidWebusbTargetFactory(navigator.usb));
}
