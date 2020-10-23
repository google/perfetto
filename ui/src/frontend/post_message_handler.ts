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

import * as m from 'mithril';

import {Actions, PostedTrace} from '../common/actions';

import {globals} from './globals';
import {showModal} from './modal';

interface PostedTraceWrapped {
  perfetto: PostedTrace;
}

// Returns whether incoming traces should be opened automatically or should
// instead require a user interaction.
function isTrustedOrigin(origin: string): boolean {
  const TRUSTED_ORIGINS = [
    'https://chrometto.googleplex.com',
    'https://uma.googleplex.com',
  ];
  if (TRUSTED_ORIGINS.includes(origin)) return true;
  if (new URL(origin).hostname.endsWith('corp.google.com')) return true;
  return false;
}


// The message handler supports loading traces from an ArrayBuffer.
// There is no other requirement than sending the ArrayBuffer as the |data|
// property. However, since this will happen across different origins, it is not
// possible for the source website to inspect whether the message handler is
// ready, so the message handler always replies to a 'PING' message with 'PONG',
// which indicates it is ready to receive a trace.
export function postMessageHandler(messageEvent: MessageEvent) {
  if (document.readyState !== 'complete') {
    console.error('Ignoring message - document not ready yet.');
    return;
  }

  if (messageEvent.source === null) {
    throw new Error('Incoming message has no source');
  }

  // This can happen if an extension tries to postMessage.
  if (messageEvent.source !== window.opener) {
    return;
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

  let postedTrace: PostedTrace;

  if (isPostedTraceWrapped(messageEvent.data)) {
    postedTrace = sanitizePostedTrace(messageEvent.data.perfetto);
  } else if (messageEvent.data instanceof ArrayBuffer) {
    postedTrace = {title: 'External trace', buffer: messageEvent.data};
  } else {
    throw new Error('Incoming message data is not in a usable format');
  }

  if (postedTrace.buffer.byteLength === 0) {
    throw new Error('Incoming message trace buffer is empty');
  }

  const openTrace = () => {
    // For external traces, we need to disable other features such as
    // downloading and sharing a trace.
    globals.frontendLocalState.localOnlyMode = true;
    globals.dispatch(Actions.openTraceFromBuffer(postedTrace));
  };

  // If the origin is trusted open the trace directly.
  if (isTrustedOrigin(messageEvent.origin)) {
    openTrace();
    return;
  }

  // If not ask the user if they expect this and trust the origin.
  showModal({
    title: 'Open trace?',
    content:
        m('div',
          m('div', `${messageEvent.origin} is trying to open a trace file.`),
          m('div', 'Do you trust the origin and want to proceed?')),
    buttons: [
      {text: 'NO', primary: true, id: 'pm_reject_trace', action: () => {}},
      {text: 'YES', primary: false, id: 'pm_open_trace', action: openTrace},
    ],
  });
}

function sanitizePostedTrace(postedTrace: PostedTrace): PostedTrace {
  const result: PostedTrace = {
    title: sanitizeString(postedTrace.title),
    buffer: postedTrace.buffer
  };
  if (postedTrace.url !== undefined) {
    result.url = sanitizeString(postedTrace.url);
  }
  return result;
}

function sanitizeString(str: string): string {
  return str.replace(/[^A-Za-z0-9.\-_#:/?=&;% ]/g, ' ');
}

// tslint:disable:no-any
function isPostedTraceWrapped(obj: any): obj is PostedTraceWrapped {
  const wrapped = obj as PostedTraceWrapped;
  if (wrapped.perfetto === undefined) {
    return false;
  }
  return wrapped.perfetto.buffer !== undefined &&
      wrapped.perfetto.title !== undefined;
}
