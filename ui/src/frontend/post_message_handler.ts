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

import {Actions} from '../common/actions';

import {globals} from './globals';

const VALID_ORIGINS = [
  'https://chrometto.googleplex.com',
  'https://uma.googleplex.com',
];

// The message handler supports loading traces from an ArrayBuffer.
// There is no other requirement than sending the ArrayBuffer as the |data|
// property. However, since this will happen across different origins, it is not
// possible for the source website to inspect whether the message handler is
// ready, so the message handler always replies to a 'PING' message with 'PONG',
// which indicates it is ready to receive a trace.
export function postMessageHandler(messageEvent: MessageEvent) {
  if (!VALID_ORIGINS.includes(messageEvent.origin)) {
    throw new Error('Invalid origin for postMessage: ' + messageEvent.origin);
  }

  if (document.readyState !== 'complete') {
    console.error('Not ready.');
    return;
  }

  if (messageEvent.source === null) {
    throw new Error('Incoming message has no source');
  }

  if (!('data' in messageEvent)) {
    throw new Error('Incoming message has no data property');
  }

  if (messageEvent.data === 'PING') {
    // Cross-origin messaging means we can't read |messageEvent.source|, but
    // it still needs to be of the correct type to be able to invoke the
    // correct version of postMessage(...).
    const windowSource = messageEvent.source as Window;
    windowSource.postMessage('PONG', messageEvent.origin);
    return;
  }

  if (!(messageEvent.data instanceof ArrayBuffer)) {
    throw new Error('Incoming message data is not an ArrayBuffer');
  }

  const buffer = messageEvent.data;
  if (buffer.byteLength === 0) {
    throw new Error('Incoming message trace buffer is empty');
  }

  // For external traces, we need to disable other features such as downloading
  // and sharing a trace.
  globals.frontendLocalState.localOnlyMode = true;

  globals.dispatch(Actions.openTraceFromBuffer({buffer}));
}
