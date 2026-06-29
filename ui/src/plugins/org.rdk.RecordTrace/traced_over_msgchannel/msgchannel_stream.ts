// Copyright 2026 Comcast Cable Communications Management, LLC
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

import {ByteStream} from '../interfaces/byte_stream';
import {defer} from '../../../base/deferred';

export class MsgChannelStream extends ByteStream {
  private msgPort: MessagePort;
  private isConnected: boolean;

  static async connect(
    srcWindow: Window,
    srcDomain: string,
    session: string,
  ): Promise<MsgChannelStream | undefined> {
    // Creates a new MessageChannel and sends one of the ports to the parent window,
    // which is expected to forward it to the traced service. The connection is
    // considered successful if the parent window responds by sending a message
    // back on the port within a reasonable time frame.
    const channel = new MessageChannel();

    srcWindow.postMessage(
      {type: 'MSGCHANNEL_CONNECT', session: session},
      srcDomain,
      [channel.port2],
    );

    const connectPromise = defer<MsgChannelStream | undefined>();

    const timeoutId = setTimeout(() => {
      channel.port1.postMessage({type: 'CLOSE'});
      channel.port1.close();
      connectPromise.resolve(undefined);
    }, 10000 /* ms */);

    channel.port1.onmessage = (e) => {
      if (e.data?.type === 'CONNECTED') {
        clearTimeout(timeoutId);
        connectPromise.resolve(new MsgChannelStream(channel.port1));
      } else if (e.data?.type === 'CONNECTION_FAILED') {
        clearTimeout(timeoutId);
        connectPromise.resolve(undefined);
      }
    };

    return connectPromise;
  }

  private constructor(port: MessagePort) {
    super();
    this.msgPort = port;
    port.onmessage = this.onPortMessage.bind(this);
    port.onmessageerror = this.onPortMessageError.bind(this);
    this.isConnected = true;
  }

  get connected(): boolean {
    return this.isConnected;
  }

  async write(data: string | Uint8Array): Promise<void> {
    if (!this.isConnected) return;

    if (typeof data === 'string') {
      this.msgPort.postMessage({type: 'DATA', data: data});
    } else {
      const copy = data.slice();
      this.msgPort.postMessage({type: 'DATA', data: copy}, [copy.buffer]);
    }
  }

  close(): void {
    if (!this.isConnected) return;
    this.isConnected = false;
    this.msgPort.postMessage({type: 'CLOSE'});
    this.msgPort.close();
    this.onClose();
  }

  [Symbol.dispose]() {
    this.close();
  }

  private async onPortMessage(ev: MessageEvent) {
    // console.log('Received message on MessagePort', ev);
    if (ev.data?.type === 'PING') {
      this.msgPort.postMessage({type: 'PONG'});
      return;
    }
    if (ev.data?.type === 'DATA' && ev.data.data instanceof ArrayBuffer) {
      this.onData(new Uint8Array(ev.data.data));
    } else if (ev.data?.type === 'CLOSED') {
      this.isConnected = false;
      this.msgPort.close();
      this.onClose();
    } else {
      console.warn('Received unexpected message on MessagePort', ev);
    }
  }

  private async onPortMessageError(ev: MessageEvent) {
    console.error('MessagePort error', ev);
    this.isConnected = false;
    this.msgPort.close();
    this.onClose();
  }
}
