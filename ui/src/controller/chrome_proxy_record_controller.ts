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

import {uint8ArrayToString} from '../base/string_utils';

import {ConsumerPortResponse, Typed} from './consumer_port_types';
import {Consumer, RpcConsumerPort} from './record_controller_interfaces';

export interface ChromeExtensionError extends Typed {
  error: string;
}

export interface ChromeExtensionStatus extends Typed {
  status: string;
}

export type ChromeExtensionMessage =
    ChromeExtensionError|ChromeExtensionStatus|ConsumerPortResponse;

function isError(obj: Typed): obj is ChromeExtensionError {
  return obj.type === 'ChromeExtensionError';
}

function isStatus(obj: Typed): obj is ChromeExtensionStatus {
  return obj.type === 'ChromeExtensionStatus';
}

// This class acts as a proxy from the record controller (running in a worker),
// to the frontend. This is needed because we can't directly talk with the
// extension from a web-worker, so we use a MessagePort to communicate with the
// frontend, that will consecutively forward it to the extension.
export class ChromeExtensionConsumerPort extends RpcConsumerPort {
  private extensionPort: MessagePort;

  constructor(extensionPort: MessagePort, consumerPortListener: Consumer) {
    super(consumerPortListener);
    this.extensionPort = extensionPort;
    this.extensionPort.onmessage = this.onExtensionMessage.bind(this);
  }

  onExtensionMessage(message: {data: ChromeExtensionMessage}) {
    if (isError(message.data)) {
      this.sendErrorMessage(message.data.error);
    } else if (isStatus(message.data)) {
      this.sendStatus(message.data.status);
    } else {
      this.sendMessage(message.data);
    }
  }

  handleCommand(method: string, requestData: Uint8Array): void {
    const buffer = uint8ArrayToString(requestData);
    // We need to encode the buffer as a string because the message port doesn't
    // fully support sending ArrayBuffers (they are converted to objects with
    // indexes as keys).
    this.extensionPort.postMessage({method, requestData: buffer});
  }
}
