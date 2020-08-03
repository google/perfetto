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

import {binaryDecode, binaryEncode} from '../base/string_utils';
import {Actions} from '../common/actions';

import {
  ConsumerPortResponse,
  isReadBuffersResponse,
  Typed
} from './consumer_port_types';
import {globals} from './globals';
import {Consumer, RpcConsumerPort} from './record_controller_interfaces';

export interface ChromeExtensionError extends Typed {
  error: string;
}

export interface ChromeExtensionStatus extends Typed {
  status: string;
}

export interface GetCategoriesResponse extends Typed {
  categories: string[];
}

export type ChromeExtensionMessage = ChromeExtensionError|ChromeExtensionStatus|
    ConsumerPortResponse|GetCategoriesResponse;

function isError(obj: Typed): obj is ChromeExtensionError {
  return obj.type === 'ChromeExtensionError';
}

function isStatus(obj: Typed): obj is ChromeExtensionStatus {
  return obj.type === 'ChromeExtensionStatus';
}

function isGetCategoriesResponse(obj: Typed): obj is GetCategoriesResponse {
  return obj.type === 'GetCategoriesResponse';
}

// This class acts as a proxy from the record controller (running in a worker),
// to the frontend. This is needed because we can't directly talk with the
// extension from a web-worker, so we use a MessagePort to communicate with the
// frontend, that will consecutively forward it to the extension.

// Rationale for the binaryEncode / binaryDecode calls below:
// Messages to/from extensions need to be JSON serializable. ArrayBuffers are
// not supported. For this reason here we use binaryEncode/Decode.
// See https://developer.chrome.com/extensions/messaging#simple

export class ChromeExtensionConsumerPort extends RpcConsumerPort {
  private extensionPort: MessagePort;

  constructor(extensionPort: MessagePort, consumer: Consumer) {
    super(consumer);
    this.extensionPort = extensionPort;
    this.extensionPort.onmessage = this.onExtensionMessage.bind(this);
  }

  onExtensionMessage(message: {data: ChromeExtensionMessage}) {
    if (isError(message.data)) {
      this.sendErrorMessage(message.data.error);
      return;
    }
    if (isStatus(message.data)) {
      this.sendStatus(message.data.status);
      return;
    }
    if (isGetCategoriesResponse(message.data)) {
      globals.dispatch(Actions.setChromeCategories(message.data));
      return;
    }

    // In this else branch message.data will be a ConsumerPortResponse.
    if (isReadBuffersResponse(message.data) && message.data.slices) {
      const slice = message.data.slices[0].data as unknown as string;
      message.data.slices[0].data = binaryDecode(slice);
    }
    this.sendMessage(message.data);
  }

  handleCommand(method: string, requestData: Uint8Array): void {
    const reqEncoded = binaryEncode(requestData);
    this.extensionPort.postMessage({method, requestData: reqEncoded});
  }
}
