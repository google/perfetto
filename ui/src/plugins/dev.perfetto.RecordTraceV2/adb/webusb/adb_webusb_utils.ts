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

export const ADB_DEVICE_FILTER = {
  classCode: 255, // USB vendor specific code
  subclassCode: 66, // Android vendor specific subclass
  protocolCode: 1, // Adb protocol
};

export interface AdbUsbInterface {
  readonly dev: USBDevice;
  readonly configurationValue: number;
  readonly usbInterfaceNumber: number;
  readonly rx: number;
  readonly tx: number;
  readonly txPacketSize: number;
}

// Returns a key that can be used to index the device in a map for idempotency
// checks.
export function usbDeviceToStr(d: USBDevice): string {
  const ver = `${d.deviceVersionMajor}.${d.deviceVersionMinor}`;
  return `${d.vendorId}:${d.productId}:${ver}:${d.serialNumber}`;
}

export function getAdbWebUsbInterface(
  device: USBDevice,
): AdbUsbInterface | undefined {
  if (!exists(device.serialNumber)) return undefined;
  const adbDeviceFilter = ADB_DEVICE_FILTER;
  for (const config of device.configurations) {
    for (const iface of config.interfaces) {
      for (const alt of iface.alternates) {
        if (
          alt.interfaceClass === adbDeviceFilter.classCode &&
          alt.interfaceSubclass === adbDeviceFilter.subclassCode &&
          alt.interfaceProtocol === adbDeviceFilter.protocolCode
        ) {
          const rxEndpoint = alt.endpoints.find(
            (e) => e.type === 'bulk' && e.direction === 'in',
          );
          const txEndpoint = alt.endpoints.find(
            (e) => e.type === 'bulk' && e.direction === 'out',
          );
          if (rxEndpoint === undefined || txEndpoint === undefined) continue;
          return {
            dev: device,
            configurationValue: config.configurationValue,
            usbInterfaceNumber: iface.interfaceNumber,
            rx: rxEndpoint.endpointNumber,
            tx: txEndpoint.endpointNumber,
            txPacketSize: txEndpoint.packetSize,
          };
        } // if (alternate)
      } // for (interface.alternates)
    } // for (configuration.interfaces)
  } // for (configurations)

  return undefined;
}
