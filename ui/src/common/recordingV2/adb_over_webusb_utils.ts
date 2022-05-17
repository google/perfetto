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

export interface UsbInterfaceAndEndpoint {
  readonly configurationValue: number;
  readonly usbInterfaceNumber: number;
  readonly endpoints: USBEndpoint[];
}

export const ADB_DEVICE_FILTER = {
  classCode: 255,    // USB vendor specific code
  subclassCode: 66,  // Android vendor specific subclass
  protocolCode: 1    // Adb protocol
};

export function findInterfaceAndEndpoint(device: USBDevice):
    UsbInterfaceAndEndpoint|undefined {
  const adbDeviceFilter = ADB_DEVICE_FILTER;
  for (const config of device.configurations) {
    for (const interface_ of config.interfaces) {
      for (const alt of interface_.alternates) {
        if (alt.interfaceClass === adbDeviceFilter.classCode &&
            alt.interfaceSubclass === adbDeviceFilter.subclassCode &&
            alt.interfaceProtocol === adbDeviceFilter.protocolCode) {
          return {
            configurationValue: config.configurationValue,
            usbInterfaceNumber: interface_.interfaceNumber,
            endpoints: alt.endpoints
          };
        }  // if (alternate)
      }    // for (interface.alternates)
    }      // for (configuration.interfaces)
  }        // for (configurations)

  return undefined;
}
