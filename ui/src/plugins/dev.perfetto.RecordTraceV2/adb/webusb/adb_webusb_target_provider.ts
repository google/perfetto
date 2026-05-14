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

import {exists} from '../../../../base/utils';
import {PreflightCheck} from '../../interfaces/connection_check';
import {AdbKeyManager} from './adb_key_manager';
import {
  ADB_DEVICE_FILTER,
  AdbUsbInterface,
  getAdbWebUsbInterface,
  usbDeviceToStr,
} from './adb_webusb_utils';
import {errResult} from '../../../../base/result';
import {RecordingTargetProvider} from '../../interfaces/recording_target_provider';
import {AdbWebusbTarget} from './adb_webusb_target';
import {EvtSource} from '../../../../base/events';

export class AdbWebusbTargetProvider implements RecordingTargetProvider {
  readonly id = 'adb_webusb';
  readonly name = 'WebUsb';
  readonly icon = 'usb';
  readonly supportedPlatforms = ['ANDROID'] as const;
  readonly description =
    'This is the easiest option to use but requires exclusive access to the ' +
    'device. If you are an android developer and use ADB, you should use the ' +
    'websocket option instead.';

  private adbKeyMgr = new AdbKeyManager();
  private targets = new Map<string, AdbWebusbTarget>();
  readonly onTargetsChanged = new EvtSource<void>();

  constructor() {
    if (!exists(navigator.usb)) return;
    navigator.usb.addEventListener('disconnect', () => this.refreshTargets());
    navigator.usb.addEventListener('connect', () => this.refreshTargets());
  }

  async listTargets(): Promise<AdbWebusbTarget[]> {
    if (!exists(navigator.usb)) return [];
    await this.refreshTargets();
    return Array.from(this.targets.values());
  }

  async pairNewTarget(): Promise<AdbWebusbTarget | undefined> {
    if (!exists(navigator.usb)) return undefined;
    let usbdev: USBDevice;
    try {
      usbdev = await navigator.usb.requestDevice({
        filters: [ADB_DEVICE_FILTER],
      });
    } catch (err) {
      if (`${err.name}` === 'NotFoundError') {
        return undefined; // The user just clicked cancel.
      }
      throw err;
    }
    const usbiface = getAdbWebUsbInterface(usbdev);
    if (usbiface === undefined) return undefined;

    const key = usbDeviceToStr(usbdev);
    this.removeTarget(key);

    // If the user re-pairs the same device, remove it from the list and keep
    // the new one.
    const newTarget = new AdbWebusbTarget(usbiface, this.adbKeyMgr);
    this.targets.set(key, newTarget);
    this.onTargetsChanged.notify();
    return newTarget;
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    if (!exists(navigator.usb)) {
      yield {
        name: 'WebUSB support',
        status: errResult(`Not supported`),
      };
    }
  }

  private async refreshTargets() {
    let triggerOnTrgetsChanged = false;
    const usbDevices = await this.listUsbDevices();
    // Find and disconnected devices.
    for (const key of this.targets.keys()) {
      if (!usbDevices.has(key)) {
        // Entry disconnected.
        this.removeTarget(key);
        triggerOnTrgetsChanged = true;
      }
    }
    for (const [key, usbiface] of usbDevices.entries()) {
      if (this.targets.has(key)) continue; // We already have this target.
      const newTarget = new AdbWebusbTarget(usbiface, this.adbKeyMgr);
      this.targets.set(key, newTarget);
      triggerOnTrgetsChanged = true;
    }
    triggerOnTrgetsChanged && this.onTargetsChanged.notify();
  }

  private removeTarget(key: string) {
    const target = this.targets.get(key);
    if (target === undefined) return;
    this.targets.delete(key);
    target.disconnect();
  }

  private async listUsbDevices(): Promise<Map<string, AdbUsbInterface>> {
    const devices = new Map<string, AdbUsbInterface>();
    // NOTE: getDevices() only returns the previously paired devices. It will
    // not list connected devices that never got paired. In order to discover
    // those we need to call navigator.usb.requestDevices() which prompts the
    // "pair device" dialog. See pairNewTarget().
    for (const dev of await navigator.usb.getDevices()) {
      const usbiface = getAdbWebUsbInterface(dev);
      if (usbiface === undefined) continue;
      const key = usbDeviceToStr(dev);
      devices.set(key, usbiface);
    }
    return devices;
  }
}
