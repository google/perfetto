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

import {findInterfaceAndEndpoint} from './adb_over_webusb_utils';
import {
  AdbConnection,
  ByteStream,
  OnDisconnectCallback,
  OnMessageCallback,
  OnStreamCloseCallback,
  OnStreamDataCallback
} from './recording_interfaces_v2';

const textEncoder = new _TextEncoder();
const textDecoder = new _TextDecoder();

export const VERSION_WITH_CHECKSUM = 0x01000000;
export const VERSION_NO_CHECKSUM = 0x01000001;
export const DEFAULT_MAX_PAYLOAD_BYTES = 256 * 1024;

export enum AdbState {
  DISCONNECTED = 0,
  // Authentication steps, see AdbConnectionOverWebUsb's handleAuthentication().
  AUTH_STEP1 = 1,
  AUTH_STEP2 = 2,
  AUTH_STEP3 = 3,

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

export class AdbConnectionOverWebusb implements AdbConnection {
  private state: AdbState = AdbState.DISCONNECTED;
  private connectingStreams = new Map<number, Deferred<AdbOverWebusbStream>>();
  private streams = new Set<AdbOverWebusbStream>();
  private maxPayload = DEFAULT_MAX_PAYLOAD_BYTES;
  private key?: CryptoKeyPair;
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
  // onStatus and onDisconnect are set to callbacks passed from the caller.
  // This happens for instance in the AndroidWebusbTarget, which instantiates
  // them with callbacks passed from the UI.
  onStatus: OnMessageCallback = () => {};
  onDisconnect: OnDisconnectCallback = (_) => {};

  constructor(private device: USBDevice) {}

  async connectSocket(path: string): Promise<AdbOverWebusbStream> {
    await this.ensureConnectionEstablished();
    const streamId = ++this.lastStreamId;
    const connectingStream = defer<AdbOverWebusbStream>();
    this.connectingStreams.set(streamId, connectingStream);
    this.sendMessage('OPEN', streamId, 0, 'localfilesystem:' + path);
    return connectingStream;
  }

  private async ensureConnectionEstablished(): Promise<void> {
    if (this.state === AdbState.CONNECTED) {
      return;
    }

    if (this.state === AdbState.DISCONNECTED) {
      await this.device.open();
      // Setup USB endpoint.
      const interfaceAndEndpoint = findInterfaceAndEndpoint(this.device);
      const {configurationValue, usbInterfaceNumber, endpoints} =
          assertExists(interfaceAndEndpoint);
      this.usbInterfaceNumber = usbInterfaceNumber;
      this.usbReadEndpoint = this.findEndpointNumber(endpoints, 'in');
      this.usbWriteEpEndpoint = this.findEndpointNumber(endpoints, 'out');
      assertTrue(this.usbReadEndpoint >= 0 && this.usbWriteEpEndpoint >= 0);

      await this.device.selectConfiguration(configurationValue);
      try {
        // This can throw if the user is also running adb on the machine.
        // The adb server takes control over the same USB endpoint.
        await this.device.claimInterface(usbInterfaceNumber);
      } catch (e) {
        // Here, we are unable to claim the interface so we don't need to
        // disconnect. However, we signal to the code that the connection ended.
        this.onDisconnect('Unable to claim adb interface.');
        // TODO(octaviant) from aosp/1918377 - look at handling adb errors
        // uniformly in the adb logic
        throw e;
      }
    }

    await this.startAdbAuth();
    if (!this.isUsbReceiveLoopRunning) {
      this.usbReceiveLoop();
    }
    const connPromise = defer<void>();
    this.pendingConnPromises.push(connPromise);
    await connPromise;
  }

  streamClose(stream: AdbOverWebusbStream): void {
    const otherStreamsQueue = this.writeQueue.filter(
        queueElement => queueElement.localStreamId !== stream.localStreamId);
    const droppedPacketCount =
        this.writeQueue.length - otherStreamsQueue.length;
    if (droppedPacketCount > 0) {
      console.debug(`Dropping ${
          droppedPacketCount} queued messages due to stream closing.`);
      this.writeQueue = otherStreamsQueue;
    }

    this.streams.delete(stream);
    if (this.streams.size === 0) {
      this.disconnect();
    }
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
  // 1.When we close the last stream of the connection. This is to prevent the
  // browser holding onto the USB interface after having finished a trace
  // recording, which would make it impossible to use "adb shell" from the same
  // machine until the browser is closed. 2.When we get a USB disconnect event.
  // This happens for instance when the device is unplugged.
  async disconnect(disconnectMessage?: string): Promise<void> {
    if (this.state === AdbState.DISCONNECTED) {
      return;
    }

    this.state = AdbState.DISCONNECTED;
    this.writeInProgress = false;

    for (const [id, stream] of this.connectingStreams.entries()) {
      stream.reject(
          `Failed to open stream with id ${id} because adb was disconnected.`);
    }
    this.connectingStreams.clear();

    this.streams.forEach(stream => stream.close());
    this.onDisconnect(disconnectMessage);

    try {
      await this.device.releaseInterface(assertExists(this.usbInterfaceNumber));
    } catch (_) {
      // We may not be able to release the interface because the device has been
      // already disconnected.
    }
    this.usbInterfaceNumber = undefined;
  }

  private async startAdbAuth(): Promise<void> {
    const KEY_SIZE = 2048;
    const keySpec = {
      name: 'RSASSA-PKCS1-v1_5',
      modulusLength: KEY_SIZE,
      publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      hash: {name: 'SHA-1'},
    };
    this.key = await crypto.subtle.generateKey(
        keySpec, /*extractable=*/ true, ['sign', 'verify']);

    const VERSION =
        this.useChecksum ? VERSION_WITH_CHECKSUM : VERSION_NO_CHECKSUM;
    this.state = AdbState.AUTH_STEP1;
    await this.sendMessage('CNXN', VERSION, this.maxPayload, 'host:1:UsbADB');
  }

  private findEndpointNumber(
      endpoints: USBEndpoint[], direction: 'out'|'in', type = 'bulk'): number {
    const ep =
        endpoints.find((ep) => ep.type === type && ep.direction === direction);

    if (ep) return ep.endpointNumber;

    throw Error(`Cannot find ${direction} endpoint`);
  }

  private async usbReceiveLoop(): Promise<void> {
    assertFalse(this.isUsbReceiveLoopRunning);
    this.isUsbReceiveLoopRunning = true;
    for (; this.state !== AdbState.DISCONNECTED;) {
      const res =
          await this.device.transferIn(this.usbReadEndpoint, ADB_MSG_SIZE);
      assertTrue(res.status === 'ok');

      const msg = AdbMsg.decodeHeader(res.data!);
      if (msg.dataLen > 0) {
        const resp =
            await this.device.transferIn(this.usbReadEndpoint, msg.dataLen);
        msg.data = new Uint8Array(
            resp.data!.buffer, resp.data!.byteOffset, resp.data!.byteLength);
      }
      if (this.useChecksum) {
        assertTrue(generateChecksum(msg.data) === msg.dataChecksum);
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
          this.state === AdbState.AUTH_STEP3) {
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
        const token = msg.data;
        if (this.state === AdbState.AUTH_STEP1) {
          // During this step, we send back the token received signed with our
          // private key. If the device has previously received our public key,
          // the dialog will not be displayed. Otherwise we will receive another
          // message ending up in AUTH_STEP3.
          this.state = AdbState.AUTH_STEP2;

          // Unfortunately this authentication as it is does NOT work, as the
          // user is asked by the device to allow auth on every interaction.
          // TODO(octaviant): fix the authentication
          const signedToken = signAdbTokenWithPrivateKey(
              assertExists(this.key).privateKey, token);
          this.sendMessage(
              'AUTH', AuthCmd.SIGNATURE, 0, new Uint8Array(signedToken));
        } else {
          // During this step, we send our public key. The dialog asking for
          // authorisation will appear on device, and if the user chooses to
          // remember our public key, it will be saved, so that the next time we
          // will only pass through AUTH_STEP1.
          this.state = AdbState.AUTH_STEP3;
          const encodedPubKey =
              await encodePubKey(assertExists(this.key).publicKey);
          this.sendMessage('AUTH', AuthCmd.RSAPUBLICKEY, 0, encodedPubKey);
          this.onStatus('Please allow USB debugging on device.');
        }
      } else if (msg.cmd === 'CNXN') {
        assertTrue(
            [AdbState.AUTH_STEP2, AdbState.AUTH_STEP3].includes(this.state));
        this.state = AdbState.CONNECTED;
        this.maxPayload = msg.arg1;

        const deviceVersion = msg.arg0;

        if (![VERSION_WITH_CHECKSUM, VERSION_NO_CHECKSUM].includes(
                deviceVersion)) {
          throw new Error(`Version ${msg.arg0} not supported.`);
        }
        this.useChecksum = deviceVersion === VERSION_WITH_CHECKSUM;
        this.state = AdbState.CONNECTED;

        // This will resolve the promises awaited by
        // "ensureConnectionEstablished".
        this.pendingConnPromises.forEach(connPromise => connPromise.resolve());
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
        this.sendMessage('OKAY', stream.localStreamId, stream.remoteStreamId);
        stream.onStreamData(msg.data);
      } else {
        this.isUsbReceiveLoopRunning = false;
        throw new Error(`Unexpected message ${msg} in state ${this.state}`);
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

    const sendPromises =
        [this.device.transferOut(this.usbWriteEpEndpoint, msgHeader.buffer)];
    if (msg.data.length > 0) {
      sendPromises.push(
          this.device.transferOut(this.usbWriteEpEndpoint, msgData.buffer));
    }
    await Promise.all(sendPromises);
  }
}

// An AdbOverWebusbStream is instantiated after the creation of a socket to the
// device. Thanks to this, we can send commands and receive their output.
// Messages are received in the main adb class, and are forwarded to an instance
// of this class based on a stream id match.
export class AdbOverWebusbStream implements ByteStream {
  private adbConnection: AdbConnectionOverWebusb;
  localStreamId: number;
  remoteStreamId = -1;

  onStreamData: OnStreamDataCallback = (_) => {};
  onStreamClose: OnStreamCloseCallback = () => {};

  constructor(
      adb: AdbConnectionOverWebusb, localStreamId: number,
      remoteStreamId: number) {
    this.adbConnection = adb;
    this.localStreamId = localStreamId;
    this.remoteStreamId = remoteStreamId;
  }

  close(): void {
    this.adbConnection.streamClose(this);
  }

  write(msg: string|Uint8Array): void {
    this.adbConnection.streamWrite(msg, this);
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

function base64StringToArray(s: string): number[] {
  const decoded = atob(s.replace(/-/g, '+').replace(/_/g, '/'));
  return [...decoded].map(char => char.charCodeAt(0));
}

const ANDROID_PUBKEY_MODULUS_SIZE = 2048;
const MODULUS_SIZE_BYTES = ANDROID_PUBKEY_MODULUS_SIZE / 8;

// RSA Public keys are encoded in a rather unique way. It's a base64 encoded
// struct of 524 bytes in total as follows (see
// libcrypto_utils/android_pubkey.c):
//
// typedef struct RSAPublicKey {
//   // Modulus length. This must be ANDROID_PUBKEY_MODULUS_SIZE.
//   uint32_t modulus_size_words;
//
//   // Precomputed montgomery parameter: -1 / n[0] mod 2^32
//   uint32_t n0inv;
//
//   // RSA modulus as a little-endian array.
//   uint8_t modulus[ANDROID_PUBKEY_MODULUS_SIZE];
//
//   // Montgomery parameter R^2 as a little-endian array of little-endian
//   words. uint8_t rr[ANDROID_PUBKEY_MODULUS_SIZE];
//
//   // RSA modulus: 3 or 65537
//   uint32_t exponent;
// } RSAPublicKey;
//
// However, the Montgomery params (n0inv and rr) are not really used, see
// comment in android_pubkey_decode() ("Note that we don't extract the
// montgomery parameters...")
async function encodePubKey(key: CryptoKey) {
  const expPubKey = await crypto.subtle.exportKey('jwk', key);
  const nArr = base64StringToArray(expPubKey.n as string).reverse();
  const eArr = base64StringToArray(expPubKey.e as string).reverse();

  const arr = new Uint8Array(3 * 4 + 2 * MODULUS_SIZE_BYTES);
  const dv = new DataView(arr.buffer);
  dv.setUint32(0, MODULUS_SIZE_BYTES / 4, true);

  // The Mongomery params (n0inv and rr) are not computed.
  dv.setUint32(4, 0 /*n0inv*/, true);
  // Modulus
  for (let i = 0; i < MODULUS_SIZE_BYTES; i++) dv.setUint8(8 + i, nArr[i]);

  // rr:
  for (let i = 0; i < MODULUS_SIZE_BYTES; i++) {
    dv.setUint8(8 + MODULUS_SIZE_BYTES + i, 0 /*rr*/);
  }
  // Exponent
  for (let i = 0; i < 4; i++) {
    dv.setUint8(8 + (2 * MODULUS_SIZE_BYTES) + i, eArr[i]);
  }
  return btoa(String.fromCharCode(...new Uint8Array(dv.buffer))) +
      ' perfetto@webusb';
}

// TODO(nicomazz): This token signature will be useful only when we save the
// generated keys. So far, we are not doing so. As a consequence, a dialog is
// displayed every time a tracing session is started.
// The reason why it has not already been implemented is that the standard
// crypto.subtle.sign function assumes that the input needs hashing, which is
// not the case for ADB, where the 20 bytes token is already hashed.
// A solution to this is implementing a custom private key signature with a js
// implementation of big integers. Maybe, wrapping the key like in the following
// CL can work:
// https://android-review.googlesource.com/c/platform/external/perfetto/+/1105354/18
function signAdbTokenWithPrivateKey(
    _privateKey: CryptoKey, token: Uint8Array): ArrayBuffer {
  // This function is not implemented.
  return token.buffer;
}
