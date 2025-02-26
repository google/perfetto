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

import {defer, Deferred} from '../../../base/deferred';
import {assertExists, assertTrue} from '../../../base/logging';
import {ResizableArrayBuffer} from '../../../base/resizable_array_buffer';
import {utf8Decode, utf8Encode} from '../../../base/string_utils';

const ANY_SIZE = -1;

/**
 * A wrapper around WebSocket with async methods.
 * It allows nicer usage with await, allowing to write async sequential code.
 * E.g.:
 * const sock = await AsyncWebsocket.connect('ws://...');
 * sock.send('command');
 * const response = await sock.waitForData(42);  // Wait to receive 42 bytes.
 * sock.send('command2');
 * const response2 = await sock.waitForData(10);  // Wait to receive 10 bytes.
 */
export class AsyncWebsocket {
  private sock?: WebSocket;
  private rxBuf = new ResizableArrayBuffer(128);
  private rxBufRead = 0;
  private rxPromise?: Deferred<Uint8Array>;
  private rxPromiseBytes = 0;

  static async connect(url: string): Promise<AsyncWebsocket | undefined> {
    const sock = new WebSocket(url);
    sock.binaryType = 'arraybuffer';

    // In case of a connection failure, there are two possible scenarios:
    // 1. The failure is immediate (e.g. due to CSP blocking access). In this
    //    case the onclose() is NOT triggered because happens before we get a
    //    chance to register the handler.
    // 2. The connection failure happens in the near future. In this case we
    //    infer a connection failure by observing on onclose().
    const readyState = sock.readyState;
    if (readyState === WebSocket.CLOSED || readyState === WebSocket.CLOSED) {
      return undefined; // Case 1.
    }
    const connectPromise = defer<AsyncWebsocket | undefined>();
    const resolveConnectPromise = (success: boolean) => {
      sock.onclose = null;
      sock.onopen = null;
      if (success) {
        connectPromise.resolve(new AsyncWebsocket(sock));
      } else {
        connectPromise.resolve(undefined);
      }
    };
    sock.onopen = () => resolveConnectPromise(true);
    sock.onclose = () => resolveConnectPromise(false);
    return connectPromise;
  }

  private constructor(sock: WebSocket) {
    this.sock = sock;
    sock.onmessage = this.onSocketMessage.bind(this);
  }

  /** Turns this back into a standard WebSocket. */
  release(): WebSocket {
    const sock = assertExists(this.sock);
    this.sock = undefined;
    sock.onmessage = null;
    sock.onopen = null;
    sock.onclose = null;
    sock.onerror = null;
    return sock;
  }

  send(data: string | ArrayBufferLike) {
    assertExists(this.sock).send(data);
  }

  waitForData(numBytes: number = ANY_SIZE): Promise<Uint8Array> {
    if (this.rxPromise !== undefined) {
      throw new Error('Another unresolved waitForData() is pending already');
    }
    const rxPromise = defer<Uint8Array>();
    if (numBytes === 0) {
      rxPromise.resolve(new Uint8Array());
      return rxPromise;
    }
    this.rxPromise = rxPromise;
    this.rxPromiseBytes = numBytes;
    this.resolveRxPromiseIfEnoughDataAvail();
    return rxPromise;
  }

  close() {
    this.sock?.close();
  }

  get connected(): boolean {
    return this.sock?.readyState === WebSocket.OPEN;
  }

  [Symbol.dispose]() {
    this.close();
  }

  async waitForString(numBytes: number = ANY_SIZE): Promise<string> {
    const data = await this.waitForData(numBytes);
    assertTrue(data.length === numBytes || numBytes === ANY_SIZE);
    return utf8Decode(data);
  }

  private async onSocketMessage(e: MessageEvent) {
    let buf: Uint8Array;
    if (typeof e.data === 'string') {
      buf = utf8Encode(e.data);
    } else {
      assertTrue(e.data instanceof ArrayBuffer);
      buf = new Uint8Array(e.data as ArrayBuffer);
    }
    this.rxBuf.append(buf);
    this.resolveRxPromiseIfEnoughDataAvail();
  }

  private resolveRxPromiseIfEnoughDataAvail() {
    if (this.rxPromise === undefined) return; // Nobody is waiting any data.
    const bytesWanted = this.rxPromiseBytes;
    const bytesAvail = this.rxBuf.size - this.rxBufRead;

    let buf: Uint8Array;
    if (bytesWanted === ANY_SIZE && bytesAvail > 0) {
      buf = this.rxBuf.get().slice();
      this.rxBuf.clear();
    } else if (bytesWanted > bytesAvail || bytesWanted === ANY_SIZE) {
      return; // Not enough data.
    } else {
      buf = this.rxBuf
        .get()
        .slice(this.rxBufRead, this.rxBufRead + this.rxPromiseBytes);
      assertTrue(buf.length === bytesWanted);
      this.rxBufRead += bytesWanted;
    }

    const rxPromise = this.rxPromise;
    this.rxPromise = undefined;
    this.rxPromiseBytes = 0;
    rxPromise.resolve(buf);
  }
}
