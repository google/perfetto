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

import {Adb, AdbStream} from './adb_interfaces';
import {ConsumerPortResponse, ReadBuffersResponse} from './consumer_port_types';
import {globals} from './globals';
import {RecordControllerMessage, uint8ArrayToBase64} from './record_controller';

enum AdbState {
  READY,
  RECORDING,
  FETCHING
}
const DEFAULT_DESTINATION_FILE = '/data/misc/perfetto-traces/trace';

export class AdbRecordController {
  // public for testing
  traceDestFile = DEFAULT_DESTINATION_FILE;
  private state = AdbState.READY;
  private adb: Adb;
  private device: USBDevice|undefined = undefined;
  private mainControllerCallback:
      (_: {data: ConsumerPortResponse|RecordControllerMessage}) => void;

  constructor(adb: Adb, mainControllerCallback: (_: {
                          data: ConsumerPortResponse
                        }) => void) {
    this.mainControllerCallback = mainControllerCallback;
    this.adb = adb;
  }

  sendMessage(message: ConsumerPortResponse|RecordControllerMessage) {
    this.mainControllerCallback({data: message});
  }

  sendErrorMessage(message: string) {
    console.error('Error in adb record controller: ', message);
    this.sendMessage({type: 'RecordControllerError', message});
  }

  sendStatus(status: string) {
    this.sendMessage({type: 'RecordControllerStatus', status});
  }

  handleCommand(method: string, params: Uint8Array) {
    // TODO(nicomazz): after having implemented the connection to the consumer
    // port socket through adb (on a real device), this class will be a simple
    // proxy.
    switch (method) {
      case 'EnableTracing':
        this.enableTracing(params);
        break;
      case 'ReadBuffers':
        this.readBuffers();
        break;
      case 'FreeBuffers':  // no-op
      case 'GetTraceStats':
      case 'DisableTracing':
        break;
      default:
        this.sendErrorMessage(`Method not recognized: ${method}`);
        break;
    }
  }

  async enableTracing(configProto: Uint8Array) {
    try {
      if (this.state !== AdbState.READY) {
        console.error('Current state of AdbRecordController is not READY');
        return;
      }
      this.device = await this.findDevice();

      if (this.device === undefined) {
        this.sendErrorMessage('No device found');
        return;
      }
      this.sendStatus(
          'Check the screen of your device and allow USB debugging.');
      await this.adb.connect(this.device);
      await this.startRecording(configProto);
      this.sendStatus('Recording in progress...');

    } catch (e) {
      this.sendErrorMessage(e.message);
    }
  }

  async startRecording(configProto: Uint8Array) {
    this.state = AdbState.RECORDING;
    const recordCommand = this.generateStartTracingCommand(configProto);
    const recordShell: AdbStream = await this.adb.shell(recordCommand);
    let response = '';
    recordShell.onData = (str, _) => response += str;
    recordShell.onClose = () => {
      if (!this.tracingEndedSuccessfully(response)) {
        this.sendErrorMessage(response);
        this.state = AdbState.READY;
        return;
      }
      this.sendMessage({type: 'EnableTracingResponse'});
    };
  }

  tracingEndedSuccessfully(response: string): boolean {
    return !response.includes(' 0 ms') && response.includes('Wrote ');
  }

  async findDevice() {
    const deviceConnected = globals.state.androidDeviceConnected;
    if (!deviceConnected) return undefined;
    const devices = await navigator.usb.getDevices();
    return devices.find(d => d.serialNumber === deviceConnected.serial);
  }

  async readBuffers() {
    console.assert(this.state === AdbState.RECORDING);
    this.state = AdbState.FETCHING;

    const readTraceShell =
        await this.adb.shell(this.generateReadTraceCommand());
    let trace = '';
    readTraceShell.onData = (str, _) => {
      // TODO(nicomazz): Since we are using base64, we can't decode the chunks
      // as they are received (without further base64 stream decoding
      // implementations). After the investigation about why without base64
      // things are not working, the chunks should be sent as they are received,
      // like in the following line.
      // this.sendMessage(this.generateChunkReadResponse(str));
      trace += str;
    };
    readTraceShell.onClose = () => {
      const decoded = atob(trace.replace(/\n/g, ''));

      this.sendMessage(
          this.generateChunkReadResponse(decoded, /* last */ true));
      this.state = AdbState.READY;
    };
  }

  generateChunkReadResponse(data: string, last = false): ReadBuffersResponse {
    return {
      type: 'ReadBuffersResponse',
      slices: [{data: data as unknown as Uint8Array, lastSliceForPacket: last}]
    };
  }

  generateReadTraceCommand(): string {
    // TODO(nicomazz): Investigate why without base64 things break.
    return `cat ${this.traceDestFile} | gzip | base64`;
  }

  generateStartTracingCommand(tracingConfig: Uint8Array) {
    const configBase64 = uint8ArrayToBase64(tracingConfig);
    const perfettoCmd = `perfetto -c - -o ${this.traceDestFile}`;
    return `echo '${configBase64}' | base64 -d | ${perfettoCmd}`;
  }
}
