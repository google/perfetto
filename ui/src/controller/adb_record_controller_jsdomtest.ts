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

import {dingus} from 'dingusjs';

import {AdbStream, MockAdb, MockAdbStream} from './adb_interfaces';
import {AdbRecordController} from './adb_record_controller';

const mainCallback = jest.fn();
const adbMock = new MockAdb();
const adbController = new AdbRecordController(adbMock, mainCallback);
const mockIntArray = new Uint8Array();

test('handleCommand', () => {
  adbController.findDevice = () => {
    return Promise.resolve(dingus<USBDevice>());
  };

  const enableTracing = jest.fn();
  adbController.enableTracing = enableTracing;
  adbController.handleCommand('EnableTracing', mockIntArray);
  expect(enableTracing).toHaveBeenCalledTimes(1);

  const readBuffers = jest.fn();
  adbController.readBuffers = readBuffers;
  adbController.handleCommand('ReadBuffers', mockIntArray);
  expect(readBuffers).toHaveBeenCalledTimes(1);

  const sendErrorMessage = jest.fn();
  adbController.sendErrorMessage = sendErrorMessage;
  adbController.handleCommand('unknown', mockIntArray);
  expect(sendErrorMessage).toBeCalledWith('Method not recognized: unknown');
});

test('enableTracing', async () => {
  const mainCallback = jest.fn();
  const adbMock = new MockAdb();
  const adbController = new AdbRecordController(adbMock, mainCallback);

  adbController.sendErrorMessage =
      jest.fn().mockImplementation(s => console.error(s));

  const findDevice = jest.fn().mockImplementation(() => {
    return Promise.resolve({} as unknown as USBDevice);
  });
  adbController.findDevice = findDevice;

  const connectToDevice =
      jest.fn().mockImplementation((_: USBDevice) => Promise.resolve());
  adbMock.connect = connectToDevice;

  const stream: AdbStream = new MockAdbStream();
  const adbShell =
      jest.fn().mockImplementation((_: string) => Promise.resolve(stream));
  adbMock.shell = adbShell;


  const sendMessage = jest.fn();
  adbController.sendMessage = sendMessage;

  adbController.generateStartTracingCommand = (_) => 'CMD';

  await adbController.enableTracing(mockIntArray);
  expect(adbShell).toBeCalledWith('CMD');
  expect(sendMessage).toHaveBeenCalledTimes(0);
  expect(findDevice).toHaveBeenCalledTimes(1);
  expect(connectToDevice).toHaveBeenCalledTimes(1);

  stream.onData('starting tracing Wrote 123 bytes', mockIntArray);
  stream.onClose();

  expect(adbController.sendErrorMessage).toHaveBeenCalledTimes(0);
  expect(sendMessage).toBeCalledWith({type: 'EnableTracingResponse'});
});


test('generateStartTracing', () => {
  adbController.traceDestFile = 'DEST';
  const testArray = new Uint8Array(1);
  testArray[0] = 65;
  const generatedCmd = adbController.generateStartTracingCommand(testArray);
  expect(generatedCmd)
      .toBe(`echo '${btoa('A')}' | base64 -d | perfetto -c - -o DEST`);
});

test('tracingEndedSuccessfully', () => {
  expect(adbController.tracingEndedSuccessfully(
             'Connected to the Perfetto traced service,\ starting tracing for \
10000 ms\nWrote 564 bytes into /data/misc/perfetto-traces/trace'))
      .toBe(true);
  expect(adbController.tracingEndedSuccessfully(
             'Connected to the Perfetto traced service, starting tracing for \
10000 ms'))
      .toBe(false);
  expect(adbController.tracingEndedSuccessfully(
             'Connected to the Perfetto traced service, starting tracing for \
0 ms'))
      .toBe(false);
});