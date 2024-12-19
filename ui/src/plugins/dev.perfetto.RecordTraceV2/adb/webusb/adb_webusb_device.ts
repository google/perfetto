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

import {defer, Deferred} from '../../../../base/deferred';
import {assertFalse, assertTrue} from '../../../../base/logging';
import {isString} from '../../../../base/object_utils';
import {hexEncode, utf8Decode, utf8Encode} from '../../../../base/string_utils';
import {exists} from '../../../../base/utils';
import {closeModal, showModal} from '../../../../widgets/modal';
import {AdbKeyManager} from './adb_key_manager';
import {AdbDevice} from '../adb_device';
import {
  encodeAdbMsg,
  encodeAdbData,
  parseAdbMsgHdr,
  AdbMsg,
  adbMsgToString,
} from '../adb_msg';
import {getAdbWebUsbInterface, AdbUsbInterface} from './adb_webusb_utils';
import {errResult, okResult, Result} from '../../../../base/result';
import {AdbWebusbStream} from './adb_webusb_stream';

const ADB_MSG_SIZE = 6 * 4; // 6 * int32.
const DEFAULT_MAX_PAYLOAD_BYTES = 256 * 1024;
const VERSION_WITH_CHECKSUM = 0x01000000;
const VERSION_NO_CHECKSUM = 0x01000001;

/**
 * This class implements the state machine required to communicate with an ADB
 * device over WebUsb. It takes a {@link USBDevice} in input and returns an
 * object suitable to run shell commands and create streams on it.
 */
export class AdbWebusbDevice extends AdbDevice {
  private lastStreamId = 0;
  private _connected = true;
  private rxLoopRunning = false;
  private streams = new Map<number, AdbWebusbStream>();
  private pendingStreams = new Map<number, PendingStream>();
  private txQueue = new Array<TxQueueEntry>();
  private txPending = false;

  /** Use {@link connect()} to obtain an instance of this class. */
  private constructor(
    private readonly usb: AdbUsbInterface,
    private readonly maxPayload: number,
    private readonly useChecksum: boolean,
  ) {
    super();
    this.usb = usb;
    // Deliberately not awaited, the rx looop will loop forever in the
    // background until we disconnect.
    this.usbRxLoop();
  }

  /**
   * Creates a new instance of this class.
   * @param usbdev the device obtained via {@link navigator.usb.requestDevice}.
   * @param adbKeyMgr an instance of the key manager.
   */
  static async connect(
    usbdev: USBDevice,
    adbKeyMgr: AdbKeyManager,
  ): Promise<Result<AdbWebusbDevice>> {
    const usb = getAdbWebUsbInterface(usbdev);
    if (usb === undefined) {
      return errResult(
        'Could not find the USB Interface. ' +
          'Try disconnecting and reconnecting the device.',
      );
    }
    if (usbdev.opened) {
      await usbdev.close();
    }
    await usbdev.open();
    using autoClose = new CloseDeviceWhenOutOfScope(usbdev);
    await usbdev.selectConfiguration(usb.configurationValue);

    try {
      await usbdev.claimInterface(usb.usbInterfaceNumber);
    } catch (err) {
      console.error(err);
      return errResult(
        'Failed to claim USB interface. Try `adb kill-server` or ' +
          'close other profiling tools and try again',
      );
    }

    const keyRes = await adbKeyMgr.getOrCreateKey();
    if (!keyRes.ok) return keyRes;
    const key = keyRes.value;

    await AdbWebusbDevice.send(
      usb,
      'CNXN',
      VERSION_NO_CHECKSUM,
      DEFAULT_MAX_PAYLOAD_BYTES,
      'host:1:WebUsb',
    );

    // At this point there are two options:
    // 1. The device accepts the key and responds with a CNXN msg.
    // 2. The device doesn't recognize us, and responds with another AUTH msg.

    // We need to have some tolerance from queued messages from previous
    // sessions, hence the 10 attempts to deal with spurious messages.
    let authAttempts = 0;
    const modalKey = 'adbauth';
    for (let attempt = 0; attempt < 10; attempt++) {
      const msg = await this.recvMsg(usb);

      if (msg.cmd === 'CNXN') {
        // Success, the device authenticated us.
        closeModal(modalKey);
        const maxPayload = msg.arg1;
        const ver = msg.arg0;
        if (ver !== VERSION_WITH_CHECKSUM && ver !== VERSION_NO_CHECKSUM) {
          return errResult(`ADB version ${ver} not supported`);
        }
        const useChecksum = ver === VERSION_WITH_CHECKSUM;
        autoClose.keepOpen = true;
        return okResult(new AdbWebusbDevice(usb, maxPayload, useChecksum));
      }

      if (msg.cmd !== 'AUTH') {
        logSpuriousMsg(msg);
        continue;
      }

      assertTrue(msg.arg0 === AuthCmd.TOKEN);
      const authAttempt = authAttempts++;
      if (authAttempt === 0) {
        // Case 1: we are presented with a nonce to sign. If the device has
        // previously received our public key, the dialog asking for user
        // confirmation will NOT be displayed.
        const signedNonce = key.sign(msg.data);
        await this.send(usb, 'AUTH', AuthCmd.SIGNATURE, 0, signedNonce);
        continue;
      }
      if (authAttempt === 1) {
        // Case 2: present our public key. This will prompt the dialog.
        await this.send(usb, 'AUTH', AuthCmd.PUBKEY, 0, key.getPublicKey());
        showModal({
          key: modalKey,
          title: 'ADB Authorization required',
          content: 'Please unlock the device and authorize the ADB connection',
        });
        continue;
      }
      break;
    }
    return errResult('ADB authorization failed');
  }

  override async createStream(svc: string): Promise<Result<AdbWebusbStream>> {
    const ps: PendingStream = {
      promise: defer<Result<AdbWebusbStream>>(),
      localId: ++this.lastStreamId,
      svc,
    };
    this.pendingStreams.set(ps.localId, ps);
    this.send('OPEN', ps.localId, 0, svc);
    return ps.promise;
  }

  override close(): void {
    this._connected = false;
    this.usb.dev.opened && this.usb.dev.close();
    this.streams.forEach((stream) => this.streamClose(stream));
  }

  get connected() {
    return this._connected;
  }

  streamWrite(
    stream: AdbWebusbStream,
    data: string | Uint8Array,
  ): Promise<void> {
    const promise = defer<void>();
    const raw = isString(data) ? utf8Encode(data) : data;
    let sent = 0;
    while (sent < raw.byteLength) {
      const chunkLen = Math.min(this.maxPayload, raw.byteLength - sent);
      const chunk = raw.subarray(sent, sent + chunkLen);
      sent += chunkLen;
      const tx: TxQueueEntry = {
        stream,
        data: chunk,
        // This is the last chunk. Attach the promise only to the last chunk.
        promise: sent === raw.byteLength ? promise : undefined,
      };
      this.txQueue.push(tx);
      if (!this.txPending) {
        assertTrue(this.txQueue.length === 1);
        this.streamWriteFromQueue(tx);
      }
    }
    return promise;
  }

  streamClose(stream: AdbWebusbStream): void {
    // Remove any pending entry from the tx queue.
    this.txQueue = this.txQueue.filter((tx) => tx.stream !== stream);
    this.send('CLSE', stream.localId, stream.remoteId);
    this.streams.delete(stream.localId);
    stream.notifyClose();
  }

  private streamWriteFromQueue(tx: TxQueueEntry) {
    assertFalse(this.txPending);
    this.txPending = true;
    this.send('WRTE', tx.stream.localId, tx.stream.remoteId, tx.data);
  }

  private async usbRxLoop(): Promise<void> {
    assertFalse(this.rxLoopRunning);
    this.rxLoopRunning = true;
    try {
      while (this._connected) {
        await this.usbRxLoopInner();
      }
    } catch (e) {
      // We allow the transferIn() in recv() to fail if we disconnected. That
      // will naturally happen in the [Symbol.dispose].
      const transferInAborted =
        e instanceof Error && e.message.includes('transfer was cancelled');
      if (!(transferInAborted && !this._connected)) {
        throw e;
      }
    } finally {
      this.rxLoopRunning = false;
      this._connected = false;
    }
  }

  private async usbRxLoopInner(): Promise<void> {
    const msg = await AdbWebusbDevice.recvMsg(this.usb);

    if (msg.cmd === 'OKAY') {
      // There are two cases here:
      // 1) This is an ACK to an OPEN (new stream).
      // 2) This is an ACK to a WRTE on an existing stream.
      const remoteStreamId = msg.arg0;
      const localStreamId = msg.arg1;
      const pendingStream = this.pendingStreams.get(localStreamId);
      if (pendingStream !== undefined) {
        // Case 1.
        this.pendingStreams.delete(localStreamId);
        const stream = new AdbWebusbStream(this, localStreamId, remoteStreamId);
        this.streams.set(localStreamId, stream);
        pendingStream.promise.resolve(okResult(stream));
      } else {
        // Case 2.
        const queuedEntry = this.popFromTxQueue(localStreamId, remoteStreamId);
        if (queuedEntry === undefined) {
          return logSpuriousMsg(msg);
        }
        this.txPending = false;
        queuedEntry.promise?.resolve();
        const next = this.txQueue[0];
        next !== undefined && this.streamWriteFromQueue(next);
      }
      return;
    } else if (msg.cmd === 'WRTE') {
      const localStreamId = msg.arg1;
      const stream = this.streams.get(localStreamId);
      if (stream === undefined) {
        return logSpuriousMsg(msg);
      }
      await this.send('OKAY', stream.localId, stream.remoteId);
      stream.onData(msg.data);
    } else if (msg.cmd === 'CLSE') {
      // Close a stream.
      const localStreamId = msg.arg1;

      // If the stream has not been opened yet, this is a failure while opening.
      const ps = this.pendingStreams.get(localStreamId);
      if (ps !== undefined) {
        this.pendingStreams.delete(localStreamId);
        ps.promise.resolve(errResult(`Stream ${ps.svc} failed to connect`));
        return;
      }

      // Otherwise the service is telling us about a stream getting closed from
      // their end (e.g. the shell:xxx command terminated).
      const stream = this.streams.get(localStreamId);
      // If we initiate the closure, the stream entry is already removed.
      if (stream !== undefined) {
        this.streams.delete(localStreamId);
        stream.notifyClose();
      }
    } else {
      console.error(`Unexpected ADB cmd ${msg.cmd} ${msg.arg0} ${msg.arg1}`);
    }
  }

  private popFromTxQueue(
    localStreamId: number,
    remoteStreamId: number,
  ): TxQueueEntry {
    for (let i = 0; i < this.txQueue.length; i++) {
      const tx = this.txQueue[i];
      if (tx.stream.localId !== localStreamId) continue;
      if (tx.stream.remoteId !== remoteStreamId) continue;
      return this.txQueue.splice(i, 1)[0];
    }
    throw new WebusbTransportError(
      `Could not find ADB queue entry L=${localStreamId}, ` +
        `R=${remoteStreamId}, TxLen=${this.txQueue.length}`,
    );
  }

  private static async recv(
    usb: AdbUsbInterface,
    len: number,
  ): Promise<DataView> {
    const res = await usb.dev.transferIn(usb.rx, len);
    if (!exists(res.data) || res.status !== 'ok') {
      throw new WebusbTransportError(`res: ${res.status}, data: ${!!res.data}`);
    }
    return res.data;
  }

  private static async recvMsg(usb: AdbUsbInterface): Promise<AdbMsg> {
    const hdrData = await this.recv(usb, ADB_MSG_SIZE);
    if (hdrData.byteLength !== ADB_MSG_SIZE) {
      const arr = new Uint8Array(hdrData.buffer);
      throw new WebusbTransportError(
        `RX spurious: ${hexEncode(arr)} ${utf8Decode(arr)}`,
      );
    }
    const hdr = parseAdbMsgHdr(hdrData);
    let payload = new Uint8Array();
    if (hdr.dataLen > 0) {
      const payloadData = await this.recv(usb, hdr.dataLen);
      payload = new Uint8Array(
        payloadData.buffer,
        payloadData.byteOffset,
        payloadData.byteLength,
      ).slice();
    }
    return {...hdr, data: payload};
  }

  private send(
    cmd: string,
    arg0: number,
    arg1: number,
    data?: Uint8Array | string,
  ): Promise<void> {
    if (!this.connected) return Promise.resolve();
    const useCksum = this.useChecksum;
    return AdbWebusbDevice.send(this.usb, cmd, arg0, arg1, data, useCksum);
  }

  private static async send(
    usb: AdbUsbInterface,
    cmd: string,
    arg0: number,
    arg1: number,
    data?: Uint8Array | string,
    useChecksum = false,
  ): Promise<void> {
    const payload = encodeAdbData(data);
    const header = encodeAdbMsg(cmd, arg0, arg1, payload, useChecksum);

    // The header and the message data must be sent consecutively. In order to
    // avoid interleaving ([hdr1] [hdr2] [data1] [data2]), we chain promises.
    const sendPromises = [usb.dev.transferOut(usb.tx, header.buffer)];
    if (payload.length > 0) {
      sendPromises.push(usb.dev.transferOut(usb.tx, payload.buffer));
      if (payload.length % usb.txPacketSize === 0) {
        // if the number of bytes transferred fits exactly into packets then
        // we need an extra zero length packet at the end.
        sendPromises.push(usb.dev.transferOut(usb.tx, new Uint8Array(0)));
      }
    }
    await Promise.all(sendPromises);
  }
}

enum AuthCmd {
  TOKEN = 1,
  SIGNATURE = 2,
  PUBKEY = 3,
}

interface TxQueueEntry {
  stream: AdbWebusbStream;
  data: Uint8Array;
  promise?: Deferred<void>;
}

interface PendingStream {
  promise: Deferred<Result<AdbWebusbStream>>;
  localId: number;
  svc: string; // The service being requested, e.g. 'shell:whoami'.
}

class WebusbTransportError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'WebusbTransportError';
  }
}

// These log messages are non-fatal because we need to tolerate the fact that
// adbd can buffer messages from previous connections (e.g. if reloading a tab)
// and won't clear the queue when we restart the flow (as one would expect).
function logSpuriousMsg(msg: AdbMsg): void {
  console.log('Spurious ADB message', adbMsgToString(msg));
}

class CloseDeviceWhenOutOfScope {
  constructor(private usbdev: USBDevice) {}
  keepOpen = false;

  [Symbol.dispose]() {
    if (this.keepOpen) return;
    if (this.usbdev.opened) {
      this.usbdev.close();
    }
  }
}
