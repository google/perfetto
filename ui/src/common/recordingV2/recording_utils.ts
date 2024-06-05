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

export const RECORDING_MODAL_DIALOG_KEY = 'recording_target';

// Begin Websocket ////////////////////////////////////////////////////////

export const WEBSOCKET_UNABLE_TO_CONNECT =
  'Unable to connect to device via websocket.';

// https://www.rfc-editor.org/rfc/rfc6455#section-7.4.1
export const WEBSOCKET_CLOSED_ABNORMALLY_CODE = 1006;

// The messages read by the adb server have their length prepended in hex.
// This method adds the length at the beginning of the message.
// Example: 'host:track-devices' -> '0012host:track-devices'
// go/codesearch/aosp-android11/system/core/adb/SERVICES.TXT
export function buildAbdWebsocketCommand(cmd: string) {
  const hdr = cmd.length.toString(16).padStart(4, '0');
  return hdr + cmd;
}

// Sample user agent for Chrome on Mac OS:
// 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36
// (KHTML, like Gecko) Chrome/104.0.0.0 Safari/537.36'
export function isMacOs(userAgent: string) {
  return userAgent.toLowerCase().includes(' mac os ');
}

// Sample user agent for Chrome on Linux:
// Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
// Chrome/105.0.0.0 Safari/537.36
export function isLinux(userAgent: string) {
  return userAgent.toLowerCase().includes(' linux ');
}
// Sample user agent for Chrome on Windows:
// Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML,
// like Gecko) Chrome/125.0.0.0 Safari/537.36
export function isWindows(userAgent: string) {
  return userAgent.toLowerCase().includes('windows ');
}

// Sample user agent for Chrome on Chrome OS:
// "Mozilla/5.0 (X11; CrOS x86_64 14816.99.0) AppleWebKit/537.36
// (KHTML, like Gecko) Chrome/103.0.5060.114 Safari/537.36"
// This condition is wider, in the unlikely possibility of different casing,
export function isCrOS(userAgent: string) {
  return userAgent.toLowerCase().includes(' cros ');
}

// End Websocket //////////////////////////////////////////////////////////

// Begin Adb //////////////////////////////////////////////////////////////

export const BINARY_PUSH_FAILURE = 'BinaryPushFailure';
export const BINARY_PUSH_UNKNOWN_RESPONSE = 'BinaryPushUnknownResponse';

// In case the device doesn't have the tracebox, we upload the latest version
// to this path.
export const TRACEBOX_DEVICE_PATH = '/data/local/tmp/tracebox';

// Experimentally, this takes 900ms on the first fetch and 20-30ms after
// because of caching.
export const TRACEBOX_FETCH_TIMEOUT = 30000;

// Message shown to the user when they need to allow authentication on the
// device in order to connect.
export const ALLOW_USB_DEBUGGING =
  'Please allow USB debugging on device and try again.';

// If the Android device has the tracing service on it (from API version 29),
// then we can connect to this consumer socket.
export const DEFAULT_TRACED_CONSUMER_SOCKET_PATH =
  'localfilesystem:/dev/socket/traced_consumer';

// If the Android device does not have the tracing service on it (before API
// version 29), we will have to push the tracebox on the device. Then, we
// can connect to this consumer socket (using it does not require system admin
// privileges).
export const CUSTOM_TRACED_CONSUMER_SOCKET_PATH =
  'localabstract:traced_consumer';

// End Adb /////////////////////////////////////////////////////////////////

// Begin Webusb ///////////////////////////////////////////////////////////

export const NO_DEVICE_SELECTED = 'No device selected.';

export interface UsbInterfaceAndEndpoint {
  readonly configurationValue: number;
  readonly usbInterfaceNumber: number;
  readonly endpoints: USBEndpoint[];
}

export const ADB_DEVICE_FILTER = {
  classCode: 255, // USB vendor specific code
  subclassCode: 66, // Android vendor specific subclass
  protocolCode: 1, // Adb protocol
};

export function findInterfaceAndEndpoint(
  device: USBDevice,
): UsbInterfaceAndEndpoint | undefined {
  const adbDeviceFilter = ADB_DEVICE_FILTER;
  for (const config of device.configurations) {
    for (const interface_ of config.interfaces) {
      for (const alt of interface_.alternates) {
        if (
          alt.interfaceClass === adbDeviceFilter.classCode &&
          alt.interfaceSubclass === adbDeviceFilter.subclassCode &&
          alt.interfaceProtocol === adbDeviceFilter.protocolCode
        ) {
          return {
            configurationValue: config.configurationValue,
            usbInterfaceNumber: interface_.interfaceNumber,
            endpoints: alt.endpoints,
          };
        } // if (alternate)
      } // for (interface.alternates)
    } // for (configuration.interfaces)
  } // for (configurations)

  return undefined;
}

// End Webusb //////////////////////////////////////////////////////////////

// Begin Chrome ///////////////////////////////////////////////////////////

export const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';
export const EXTENSION_URL = `https://chrome.google.com/webstore/detail/perfetto-ui/${EXTENSION_ID}`;
export const EXTENSION_NAME = 'Chrome extension';
export const EXTENSION_NOT_INSTALLED = `To trace Chrome from the Perfetto UI, you need to install our
    ${EXTENSION_URL} and then reload this page.`;

export const MALFORMED_EXTENSION_MESSAGE = 'Malformed extension message.';
export const BUFFER_USAGE_NOT_ACCESSIBLE = 'Buffer usage not accessible';
export const BUFFER_USAGE_INCORRECT_FORMAT =
  'The buffer usage data has am incorrect format';

// End Chrome /////////////////////////////////////////////////////////////

// Begin Traced //////////////////////////////////////////////////////////

export const RECORDING_IN_PROGRESS = 'Recording in progress';
export const PARSING_UNKNWON_REQUEST_ID = 'Unknown request id';
export const PARSING_UNABLE_TO_DECODE_METHOD = 'Unable to decode method';
export const PARSING_UNRECOGNIZED_PORT = 'Unrecognized consumer port response';
export const PARSING_UNRECOGNIZED_MESSAGE = 'Unrecognized frame message';

// End Traced ///////////////////////////////////////////////////////////
