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

import {parseWebsocketResponse} from './android_websocket_target_factory';

test('parse device disconnection', () => {
  const message = '001702121FQC20XXXX\toffline\n';
  const response = parseWebsocketResponse(message);
  expect(response.messageRemainder).toEqual('');
  expect(response.listedDevices.length).toEqual(1);
  expect(response.listedDevices[0].serialNumber).toEqual('02121FQC20XXXX');
  expect(response.listedDevices[0].connectionState).toEqual('offline');
});

test('parse two devices connected in the same message', () => {
  const message = '003202121FQC20XXXX\tdevice\n06131FDD40YYYY\tunauthorized\n';
  const response = parseWebsocketResponse(message);
  expect(response.messageRemainder).toEqual('');
  expect(response.listedDevices.length).toEqual(2);
  expect(response.listedDevices[0].serialNumber).toEqual('02121FQC20XXXX');
  expect(response.listedDevices[0].connectionState).toEqual('device');
  expect(response.listedDevices[1].serialNumber).toEqual('06131FDD40YYYY');
  expect(response.listedDevices[1].connectionState).toEqual('unauthorized');
});

test('parse device connection in multiple messages', () => {
  const message = '001702121FQC20XXXX\toffline\n001602121FQC20XXXX\tdevice\n' +
      '001602121FQC20XXXX\tdevice\n';
  const response = parseWebsocketResponse(message);
  expect(response.messageRemainder).toEqual('');
  expect(response.listedDevices.length).toEqual(1);
  expect(response.listedDevices[0].serialNumber).toEqual('02121FQC20XXXX');
  expect(response.listedDevices[0].connectionState).toEqual('device');
});

test('parse with remainder', () => {
  const remainder = 'FFFFsome_other_stuff';
  const message = `001602121FQC20XXXX\tdevice\n${remainder}`;
  const response = parseWebsocketResponse(message);
  expect(response.messageRemainder).toEqual(remainder);
  expect(response.listedDevices.length).toEqual(1);
  expect(response.listedDevices[0].serialNumber).toEqual('02121FQC20XXXX');
  expect(response.listedDevices[0].connectionState).toEqual('device');
});
