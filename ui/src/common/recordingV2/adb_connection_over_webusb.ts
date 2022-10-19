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

import {_TextDecoder, _TextEncoder} from 'custom_utils';

import {defer, Deferred} from '../../base/deferred';
import {assertExists, assertFalse, assertTrue} from '../../base/logging';
import {CmdType} from '../../controller/adb_interfaces';

import {AdbConnectionImpl} from './adb_connection_impl';
import {AdbKeyManager, maybeStoreKey} from './auth/adb_key_manager';
import {
  RecordingError,
  wrapRecordingError,
} from './recording_error_handling';
import {
  ByteStream,
  OnStreamCloseCallback,
  OnStreamDataCallback,
} from './recording_interfaces_v2';
import {ALLOW_USB_DEBUGGING, findInterfaceAndEndpoint} from './recording_utils';

const textEncoder = new _TextEncoder();
const textDecoder = new _TextDecoder();

export const VERSION_WITH_CHECKSUM = 0x01000000;
export const VERSION_NO_CHECKSUM = 0x01000001;
export const DEFAULT_MAX_PAYLOAD_BYTES = 256 * 1024;

export enum AdbState {
  DISCONNECTED = 0,
  // Authentication steps, see AdbConnectionOverWebUsb's handleAuthentication().
  AUTH_STARTED = 1,
  AUTH_WITH_PRIVATE = 2,
  AUTH_WITH_PUBLIC = 3,

  CONNECTED = 4,
}

enum AuthCmd {
  TOKEN = 1,
  SIGNATURE = 2,
  RSAPUBLICKEY = 3,
}

function generateChecksum(data: Uint8Array): number {
  let res = 0;
  for (let i = 0; i < data.byteLength; i++) res += data[i];
  return res & 0xFFFFFFFF;
}

// Message to be written to the adb connection. Contains the message itself
// and the corresponding stream identifier.
interface WriteQueueElement {
  message: Uint8Array;
  localStreamId: number;
}

export class AdbConnectionOverWebusb extends AdbConnectionImpl {
  private state: AdbState = AdbState.DISCONNECTED;
  private connectingStreams = new Map<number, Deferred<AdbOverWebusbStream>>();
  private streams = new Set<AdbOverWebusbStream>();
  private maxPayload = DEFAULT_MAX_PAYLOAD_BYTES;
  private writeInProgress = false;
  private writeQueue: WriteQueueElement[] = [];

  // Devices after Dec 2017 don't use checksum. This will be auto-detected
  // during the connection.
  private useChecksum = true;

  private lastStreamId = 0;
  private usbInterfaceNumber?: number;
  private usbReadEndpoint = -1;
  private usbWriteEpEndpoint = -1;
  private isUsbReceiveLoopRunning = false;

  private pendingConnPromises: Array<Deferred<void>> = [];

  // We use a key pair for authenticating with the device, which we do in
  // two ways:
  // - Firstly, signing with the private key.
  // - Secondly, sending over the public key (at which point the device asks the
  //   user for permissions).
  // Once we've sent the public key, for future recordings we only need to
  // sign with the private key, so the user doesn't need to give permissions
  // again.
  constructor(private device: USBDevice, private keyManager: AdbKeyManager) {
    super();
  }


  shell(cmd: string): Promise<AdbOverWebusbStream> {
    return this.openStream('shell:' + cmd);
  }

  connectSocket(path: string): Promise<AdbOverWebusbStream> {
    return this.openStream(path);
  }

  async canConnectWithoutContention(): Promise<boolean> {
    await this.device.open();
    const usbInterfaceNumber = await this.setupUsbInterface();
    try {
      await this.device.claimInterface(usbInterfaceNumber);
      await this.device.releaseInterface(usbInterfaceNumber);
      return true;
    } catch (e) {
      return false;
    }
  }

  protected async openStream(destination: string):
      Promise<AdbOverWebusbStream> {
    const streamId = ++this.lastStreamId;
    const connectingStream = defer<AdbOverWebusbStream>();
    this.connectingStreams.set(streamId, connectingStream);
    // We create the stream before trying to establish the connection, so
    // that if we fail to connect, we will reject the connecting stream.
    await this.ensureConnectionEstablished();
    await this.sendMessage('OPEN', streamId, 0, destination);
    return connectingStream;
  }

  private async ensureConnectionEstablished(): Promise<void> {
    if (this.state === AdbState.CONNECTED) {
      return;
    }

    if (this.state === AdbState.DISCONNECTED) {
      await this.device.open();
      if (!(await this.canConnectWithoutContention())) {
        await this.device.reset();
      }
      const usbInterfaceNumber = await this.setupUsbInterface();
      await this.device.claimInterface(usbInterfaceNumber);
    }

    await this.startAdbAuth();
    if (!this.isUsbReceiveLoopRunning) {
      this.usbReceiveLoop();
    }
    const connPromise = defer<void>();
    this.pendingConnPromises.push(connPromise);
    await connPromise;
  }

  private async setupUsbInterface(): Promise<number> {
    const interfaceAndEndpoint = findInterfaceAndEndpoint(this.device);
    // `findInterfaceAndEndpoint` will always return a non-null value because
    // we check for this in 'android_webusb_target_factory'. If no interface and
    // endpoints are found, we do not create a target, so we can not connect to
    // it, so we will never reach this logic.
    const {configurationValue, usbInterfaceNumber, endpoints} =
        assertExists(interfaceAndEndpoint);
    this.usbInterfaceNumber = usbInterfaceNumber;
    this.usbReadEndpoint = this.findEndpointNumber(endpoints, 'in');
    this.usbWriteEpEndpoint = this.findEndpointNumber(endpoints, 'out');
    assertTrue(this.usbReadEndpoint >= 0 && this.usbWriteEpEndpoint >= 0);
    await this.device.selectConfiguration(configurationValue);
    return usbInterfaceNumber;
  }

  async streamClose(stream: AdbOverWebusbStream): Promise<void> {
    const otherStreamsQueue = this.writeQueue.filter(
        (queueElement) => queueElement.localStreamId !== stream.localStreamId);
    const droppedPacketCount =
        this.writeQueue.length - otherStreamsQueue.length;
    if (droppedPacketCount > 0) {
      console.debug(`Dropping ${
          droppedPacketCount} queued messages due to stream closing.`);
      this.writeQueue = otherStreamsQueue;
    }

    this.streams.delete(stream);
    if (this.streams.size === 0) {
      // We disconnect BEFORE calling `signalStreamClosed`. Otherwise, there can
      // be a race condition:
      // Stream A: streamA.onStreamClose
      // Stream B: device.open
      // Stream A: device.releaseInterface
      // Stream B: device.transferOut -> CRASH
      await this.disconnect();
    }
    stream.signalStreamClosed();
  }

  streamWrite(msg: string|Uint8Array, stream: AdbOverWebusbStream): void {
    const raw = (typeof msg === 'string') ? textEncoder.encode(msg) : msg;
    if (this.writeInProgress) {
      this.writeQueue.push({message: raw, localStreamId: stream.localStreamId});
      return;
    }
    this.writeInProgress = true;
    this.sendMessage('WRTE', stream.localStreamId, stream.remoteStreamId, raw);
  }

  // We disconnect in 2 cases:
  // 1. When we close the last stream of the connection. This is to prevent the
  // browser holding onto the USB interface after having finished a trace
  // recording, which would make it impossible to use "adb shell" from the same
  // machine until the browser is closed.
  // 2. When we get a USB disconnect event. This happens for instance when the
  // device is unplugged.
  async disconnect(disconnectMessage?: string): Promise<void> {
    if (this.state === AdbState.DISCONNECTED) {
      return;
    }
    // Clear the resources in a synchronous method, because this can be used
    // for error handling callbacks as well.
    this.reachDisconnectState(disconnectMessage);

    // We have already disconnected so there is no need to pass a callback
    // which clears resources or notifies the user into 'wrapRecordingError'.
    await wrapRecordingError(
        this.device.releaseInterface(assertExists(this.usbInterfaceNumber)),
        () => {});
    this.usbInterfaceNumber = undefined;
  }

  // This is a synchronous method which clears all resources.
  // It can be used as a callback for error handling.
  reachDisconnectState(disconnectMessage?: string): void {
    // We need to delete the streams BEFORE checking the Adb state because:
    //
    // We create streams before changing the Adb state from DISCONNECTED.
    // In case we can not claim the device, we will create a stream, but fail
    // to connect to the WebUSB device so the state will remain DISCONNECTED.
    const streamsToDelete = this.connectingStreams.entries();
    // Clear the streams before rejecting so we are not caught in a loop of
    // handling promise rejections.
    this.connectingStreams.clear();
    for (const [id, stream] of streamsToDelete) {
      stream.reject(
          `Failed to open stream with id ${id} because adb was disconnected.`);
    }

    if (this.state === AdbState.DISCONNECTED) {
      return;
    }

    this.state = AdbState.DISCONNECTED;
    this.writeInProgress = false;

    this.writeQueue = [];

    this.streams.forEach((stream) => stream.close());
    this.onDisconnect(disconnectMessage);
  }

  private async startAdbAuth(): Promise<void> {
    const VERSION =
        this.useChecksum ? VERSION_WITH_CHECKSUM : VERSION_NO_CHECKSUM;
    this.state = AdbState.AUTH_STARTED;
    await this.sendMessage('CNXN', VERSION, this.maxPayload, 'host:1:UsbADB');
  }

  private findEndpointNumber(
      endpoints: USBEndpoint[], direction: 'out'|'in', type = 'bulk'): number {
    const ep =
        endpoints.find((ep) => ep.type === type && ep.direction === direction);

    if (ep) return ep.endpointNumber;

    throw new RecordingError(`Cannot find ${direction} endpoint`);
  }

  private async usbReceiveLoop(): Promise<void> {
    assertFalse(this.isUsbReceiveLoopRunning);
    this.isUsbReceiveLoopRunning = true;
    for (; this.state !== AdbState.DISCONNECTED;) {
      const res = await this.wrapUsb(
          this.device.transferIn(this.usbReadEndpoint, ADB_MSG_SIZE));
      if (!res) {
        this.isUsbReceiveLoopRunning = false;
        return;
      }
      if (res.status !== 'ok') {
        // Log and ignore messages with invalid status. These can occur
        // when the device is connected/disconnected repeatedly.
        console.error(
            `Received message with unexpected status '${res.status}'`);
        continue;
      }

      const msg = AdbMsg.decodeHeader(res.data!);
      if (msg.dataLen > 0) {
        const resp = await this.wrapUsb(
            this.device.transferIn(this.usbReadEndpoint, msg.dataLen));
        if (!resp) {
          this.isUsbReceiveLoopRunning = false;
          return;
        }
        msg.data = new Uint8Array(
            resp.data!.buffer, resp.data!.byteOffset, resp.data!.byteLength);
      }

      if (this.useChecksum && generateChecksum(msg.data) !== msg.dataChecksum) {
        // We ignore messages with an invalid checksum. These sometimes appear
        // when the page is re-loaded in a middle of a recording.
        continue;
      }
      // The server can still send messages streams for previous streams.
      // This happens for instance if we record, reload the recording page and
      // then record again. We can also receive a 'WRTE' or 'OKAY' after
      // we have sent a 'CLSE' and marked the state as disconnected.
      if ((msg.cmd === 'CLSE' || msg.cmd === 'WRTE') &&
          !this.getStreamForLocalStreamId(msg.arg1)) {
        continue;
      } else if (
          msg.cmd === 'OKAY' && !this.connectingStreams.has(msg.arg1) &&
          !this.getStreamForLocalStreamId(msg.arg1)) {
        continue;
      } else if (
          msg.cmd === 'AUTH' && msg.arg0 === AuthCmd.TOKEN &&
          this.state === AdbState.AUTH_WITH_PUBLIC) {
        // If we start a recording but fail because of a faulty physical
        // connection to the device, when we start a new recording, we will
        // received multiple AUTH tokens, of which we should ignore all but
        // one.
        continue;
      }

      // handle the ADB message from the device
      if (msg.cmd === 'CLSE') {
        assertExists(this.getStreamForLocalStreamId(msg.arg1)).close();
      } else if (msg.cmd === 'AUTH' && msg.arg0 === AuthCmd.TOKEN) {
        const key = await this.keyManager.getKey();
        if (this.state === AdbState.AUTH_STARTED) {
          // During this step, we send back the token received signed with our
          // private key. If the device has previously received our public key,
          // the dialog asking for user confirmation will not be displayed on
          // the device.
          this.state = AdbState.AUTH_WITH_PRIVATE;
          await this.sendMessage(
              'AUTH', AuthCmd.SIGNATURE, 0, key.sign(msg.data));
        } else {
          // If our signature with the private key is not accepted by the
          // device, we generate a new keypair and send the public key.
          this.state = AdbState.AUTH_WITH_PUBLIC;
          await this.sendMessage(
              'AUTH', AuthCmd.RSAPUBLICKEY, 0, key.getPublicKey() + '\0');
          this.onStatus(ALLOW_USB_DEBUGGING);
          await maybeStoreKey(key);
        }
      } else if (msg.cmd === 'CNXN') {
        assertTrue(
            [AdbState.AUTH_WITH_PRIVATE, AdbState.AUTH_WITH_PUBLIC].includes(
                this.state));
        this.state = AdbState.CONNECTED;
        this.maxPayload = msg.arg1;

        const deviceVersion = msg.arg0;

        if (![VERSION_WITH_CHECKSUM, VERSION_NO_CHECKSUM].includes(
                deviceVersion)) {
          throw new RecordingError(`Version ${msg.arg0} not supported.`);
        }
        this.useChecksum = deviceVersion === VERSION_WITH_CHECKSUM;
        this.state = AdbState.CONNECTED;

        // This will resolve the promises awaited by
        // "ensureConnectionEstablished".
        this.pendingConnPromises.forEach(
            (connPromise) => connPromise.resolve());
        this.pendingConnPromises = [];
      } else if (msg.cmd === 'OKAY') {
        if (this.connectingStreams.has(msg.arg1)) {
          const connectingStream =
              assertExists(this.connectingStreams.get(msg.arg1));
          const stream = new AdbOverWebusbStream(this, msg.arg1, msg.arg0);
          this.streams.add(stream);
          this.connectingStreams.delete(msg.arg1);
          connectingStream.resolve(stream);
        } else {
          assertTrue(this.writeInProgress);
          this.writeInProgress = false;
          for (; this.writeQueue.length;) {
            // We go through the queued writes and choose the first one
            // corresponding to a stream that's still active.
            const queuedElement = assertExists(this.writeQueue.shift());
            const queuedStream =
                this.getStreamForLocalStreamId(queuedElement.localStreamId);
            if (queuedStream) {
              queuedStream.write(queuedElement.message);
              break;
            }
          }
        }
      } else if (msg.cmd === 'WRTE') {
        const stream = assertExists(this.getStreamForLocalStreamId(msg.arg1));
        await this.sendMessage(
            'OKAY', stream.localStreamId, stream.remoteStreamId);
        stream.signalStreamData(msg.data);
      } else {
        this.isUsbReceiveLoopRunning = false;
        throw new RecordingError(
            `Unexpected message ${msg} in state ${this.state}`);
      }
    }
    this.isUsbReceiveLoopRunning = false;
  }

  private getStreamForLocalStreamId(localStreamId: number): AdbOverWebusbStream
      |undefined {
    for (const stream of this.streams) {
      if (stream.localStreamId === localStreamId) {
        return stream;
      }
    }
    return undefined;
  }

  //  The header and the message data must be sent consecutively. Using 2 awaits
  //  Another message can interleave after the first header has been sent,
  //  resulting in something like [header1] [header2] [data1] [data2];
  //  In this way we are waiting both promises to be resolved before continuing.
  private async sendMessage(
      cmd: CmdType, arg0: number, arg1: number,
      data?: Uint8Array|string): Promise<void> {
    const msg =
        AdbMsg.create({cmd, arg0, arg1, data, useChecksum: this.useChecksum});

    const msgHeader = msg.encodeHeader();
    const msgData = msg.data;
    assertTrue(
        msgHeader.length <= this.maxPayload &&
        msgData.length <= this.maxPayload);

    const sendPromises = [this.wrapUsb(
        this.device.transferOut(this.usbWriteEpEndpoint, msgHeader.buffer))];
    if (msg.data.length > 0) {
      sendPromises.push(this.wrapUsb(
          this.device.transferOut(this.usbWriteEpEndpoint, msgData.buffer)));
    }
    await Promise.all(sendPromises);
  }

  private wrapUsb<T>(promise: Promise<T>): Promise<T|undefined> {
    return wrapRecordingError(promise, this.reachDisconnectState.bind(this));
  }
}

// An AdbOverWebusbStream is instantiated after the creation of a socket to the
// device. Thanks to this, we can send commands and receive their output.
// Messages are received in the main adb class, and are forwarded to an instance
// of this class based on a stream id match.
export class AdbOverWebusbStream implements ByteStream {
  private adbConnection: AdbConnectionOverWebusb;
  private _isConnected: boolean;
  private onStreamDataCallbacks: OnStreamDataCallback[] = [];
  private onStreamCloseCallbacks: OnStreamCloseCallback[] = [];
  localStreamId: number;
  remoteStreamId = -1;

  constructor(
      adb: AdbConnectionOverWebusb, localStreamId: number,
      remoteStreamId: number) {
    this.adbConnection = adb;
    this.localStreamId = localStreamId;
    this.remoteStreamId = remoteStreamId;
    // When the stream is created, the connection has been already established.
    this._isConnected = true;
  }

  addOnStreamDataCallback(onStreamData: OnStreamDataCallback): void {
    this.onStreamDataCallbacks.push(onStreamData);
  }

  addOnStreamCloseCallback(onStreamClose: OnStreamCloseCallback): void {
    this.onStreamCloseCallbacks.push(onStreamClose);
  }

  // Used by the connection object to signal newly received data, not exposed
  // in the interface.
  signalStreamData(data: Uint8Array): void {
    for (const onStreamData of this.onStreamDataCallbacks) {
      onStreamData(data);
    }
  }

  // Used by the connection object to signal the stream is closed, not exposed
  // in the interface.
  signalStreamClosed(): void {
    for (const onStreamClose of this.onStreamCloseCallbacks) {
      onStreamClose();
    }
    this.onStreamDataCallbacks = [];
    this.onStreamCloseCallbacks = [];
  }


  close(): void {
    this.closeAndWaitForTeardown();
  }

  async closeAndWaitForTeardown(): Promise<void> {
    this._isConnected = false;
    await this.adbConnection.streamClose(this);
  }

  write(msg: string|Uint8Array): void {
    this.adbConnection.streamWrite(msg, this);
  }

  isConnected(): boolean {
    return this._isConnected;
  }
}

const ADB_MSG_SIZE = 6 * 4;  // 6 * int32.

class AdbMsg {
  data: Uint8Array;
  readonly cmd: CmdType;
  readonly arg0: number;
  readonly arg1: number;
  readonly dataLen: number;
  readonly dataChecksum: number;
  readonly useChecksum: boolean;

  constructor(
      cmd: CmdType, arg0: number, arg1: number, dataLen: number,
      dataChecksum: number, useChecksum = false) {
    assertTrue(cmd.length === 4);
    this.cmd = cmd;
    this.arg0 = arg0;
    this.arg1 = arg1;
    this.dataLen = dataLen;
    this.data = new Uint8Array(dataLen);
    this.dataChecksum = dataChecksum;
    this.useChecksum = useChecksum;
  }

  static create({cmd, arg0, arg1, data, useChecksum = true}: {
    cmd: CmdType; arg0: number; arg1: number;
    data?: Uint8Array | string;
    useChecksum?: boolean;
  }): AdbMsg {
    const encodedData = this.encodeData(data);
    const msg = new AdbMsg(cmd, arg0, arg1, encodedData.length, 0, useChecksum);
    msg.data = encodedData;
    return msg;
  }

  get dataStr() {
    return textDecoder.decode(this.data);
  }

  toString() {
    return `${this.cmd} [${this.arg0},${this.arg1}] ${this.dataStr}`;
  }

  // A brief description of the message can be found here:
  // https://android.googlesource.com/platform/system/core/+/master/adb/protocol.txt
  //
  // struct amessage {
  //     uint32_t command;    // command identifier constant
  //     uint32_t arg0;       // first argument
  //     uint32_t arg1;       // second argument
  //     uint32_t data_length;// length of payload (0 is allowed)
  //     uint32_t data_check; // checksum of data payload
  //     uint32_t magic;      // command ^ 0xffffffff
  // };
  static decodeHeader(dv: DataView): AdbMsg {
    assertTrue(dv.byteLength === ADB_MSG_SIZE);
    const cmd = textDecoder.decode(dv.buffer.slice(0, 4)) as CmdType;
    const cmdNum = dv.getUint32(0, true);
    const arg0 = dv.getUint32(4, true);
    const arg1 = dv.getUint32(8, true);
    const dataLen = dv.getUint32(12, true);
    const dataChecksum = dv.getUint32(16, true);
    const cmdChecksum = dv.getUint32(20, true);
    assertTrue(cmdNum === (cmdChecksum ^ 0xFFFFFFFF));
    return new AdbMsg(cmd, arg0, arg1, dataLen, dataChecksum);
  }

  encodeHeader(): Uint8Array {
    const buf = new Uint8Array(ADB_MSG_SIZE);
    const dv = new DataView(buf.buffer);
    const cmdBytes: Uint8Array = textEncoder.encode(this.cmd);
    const rawMsg = AdbMsg.encodeData(this.data);
    const checksum = this.useChecksum ? generateChecksum(rawMsg) : 0;
    for (let i = 0; i < 4; i++) dv.setUint8(i, cmdBytes[i]);

    dv.setUint32(4, this.arg0, true);
    dv.setUint32(8, this.arg1, true);
    dv.setUint32(12, rawMsg.byteLength, true);
    dv.setUint32(16, checksum, true);
    dv.setUint32(20, dv.getUint32(0, true) ^ 0xFFFFFFFF, true);

    return buf;
  }

  static encodeData(data?: Uint8Array|string): Uint8Array {
    if (data === undefined) return new Uint8Array([]);
    if (typeof data === 'string') return textEncoder.encode(data + '\0');
    return data;
  }
}
