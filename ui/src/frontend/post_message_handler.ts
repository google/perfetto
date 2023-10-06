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

import m from 'mithril';

import {Actions, PostedScrollToRange, PostedTrace} from '../common/actions';

import {initCssConstants} from './css_constants';
import {globals} from './globals';
import {toggleHelp} from './help_modal';
import {showModal} from './modal';
import {focusHorizontalRange} from './scroll_helper';

const TRUSTED_ORIGINS_KEY = 'trustedOrigins';

interface PostedTraceWrapped {
  perfetto: PostedTrace;
}

interface PostedScrollToRangeWrapped {
  perfetto: PostedScrollToRange;
}

// Returns whether incoming traces should be opened automatically or should
// instead require a user interaction.
function isTrustedOrigin(origin: string): boolean {
  const TRUSTED_ORIGINS = [
    'https://chrometto.googleplex.com',
    'https://uma.googleplex.com',
    'https://android-build.googleplex.com',
  ];
  if (origin === window.origin) return true;
  if (TRUSTED_ORIGINS.includes(origin)) return true;
  if (isUserTrustedOrigin(origin)) return true;

  const hostname = new URL(origin).hostname;
  if (hostname.endsWith('corp.google.com')) return true;
  if (hostname === 'localhost' || hostname === '127.0.0.1') return true;
  return false;
}

// Returns whether the user saved this as an always-trusted origin.
function isUserTrustedOrigin(hostname: string): boolean {
  const trustedOrigins = window.localStorage.getItem(TRUSTED_ORIGINS_KEY);
  if (trustedOrigins === null) return false;
  try {
    return JSON.parse(trustedOrigins).includes(hostname);
  } catch {
    return false;
  }
}

// Saves the given hostname as a trusted origin.
// This is used for user convenience: if it fails for any reason, it's not a
// big deal.
function saveUserTrustedOrigin(hostname: string) {
  const s = window.localStorage.getItem(TRUSTED_ORIGINS_KEY);
  let origins: string[];
  try {
    origins = JSON.parse(s || '[]');
    if (origins.includes(hostname)) return;
    origins.push(hostname);
    window.localStorage.setItem(TRUSTED_ORIGINS_KEY, JSON.stringify(origins));
  } catch (e) {
    console.warn('unable to save trusted origins to localStorage', e);
  }
}

// Returns whether we should ignore a given message based on the value of
// the 'perfettoIgnore' field in the event data.
function shouldGracefullyIgnoreMessage(messageEvent: MessageEvent) {
  return messageEvent.data.perfettoIgnore === true;
}

// The message handler supports loading traces from an ArrayBuffer.
// There is no other requirement than sending the ArrayBuffer as the |data|
// property. However, since this will happen across different origins, it is not
// possible for the source website to inspect whether the message handler is
// ready, so the message handler always replies to a 'PING' message with 'PONG',
// which indicates it is ready to receive a trace.
export function postMessageHandler(messageEvent: MessageEvent) {
  if (shouldGracefullyIgnoreMessage(messageEvent)) {
    // This message should not be handled in this handler,
    // because it will be handled elsewhere.
    return;
  }

  if (messageEvent.origin === 'https://tagassistant.google.com') {
    // The GA debugger, does a window.open() and sends messages to the GA
    // script. Ignore them.
    return;
  }

  if (document.readyState !== 'complete') {
    console.error('Ignoring message - document not ready yet.');
    return;
  }

  const fromOpener = messageEvent.source === window.opener;
  const fromIframeHost = messageEvent.source === window.parent;
  // This adds support for the folowing flow:
  // * A (page that whats to open a trace in perfetto) opens B
  // * B (does something to get the traceBuffer)
  // * A is navigated to Perfetto UI
  // * B sends the traceBuffer to A
  // * closes itself
  const fromOpenee = (messageEvent.source as WindowProxy).opener === window;

  if (messageEvent.source === null ||
      !(fromOpener || fromIframeHost || fromOpenee)) {
    // This can happen if an extension tries to postMessage.
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

  if (messageEvent.data === 'SHOW-HELP') {
    toggleHelp();
    return;
  }

  if (messageEvent.data === 'RELOAD-CSS-CONSTANTS') {
    initCssConstants();
    return;
  }

  let postedScrollToRange: PostedScrollToRange;
  if (isPostedScrollToRange(messageEvent.data)) {
    postedScrollToRange = messageEvent.data.perfetto;
    scrollToTimeRange(postedScrollToRange);
    return;
  }

  let postedTrace: PostedTrace;
  let keepApiOpen = false;
  if (isPostedTraceWrapped(messageEvent.data)) {
    postedTrace = sanitizePostedTrace(messageEvent.data.perfetto);
    if (postedTrace.keepApiOpen) {
      keepApiOpen = true;
    }
  } else if (messageEvent.data instanceof ArrayBuffer) {
    postedTrace = {title: 'External trace', buffer: messageEvent.data};
  } else {
    console.warn(
        'Unknown postMessage() event received. If you are trying to open a ' +
        'trace via postMessage(), this is a bug in your code. If not, this ' +
        'could be due to some Chrome extension.');
    console.log('origin:', messageEvent.origin, 'data:', messageEvent.data);
    return;
  }

  if (postedTrace.buffer.byteLength === 0) {
    throw new Error('Incoming message trace buffer is empty');
  }

  if (!keepApiOpen) {
    /* Removing this event listener to avoid callers posting the trace multiple
     * times. If the callers add an event listener which upon receiving 'PONG'
     * posts the trace to ui.perfetto.dev, the callers can receive multiple
     * 'PONG' messages and accidentally post the trace multiple times. This was
     * part of the cause of b/182502595.
     */
    window.removeEventListener('message', postMessageHandler);
  }

  const openTrace = () => {
    // For external traces, we need to disable other features such as
    // downloading and sharing a trace.
    postedTrace.localOnly = true;
    globals.dispatch(Actions.openTraceFromBuffer(postedTrace));
  };

  const trustAndOpenTrace = () => {
    saveUserTrustedOrigin(messageEvent.origin);
    openTrace();
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
      {text: 'No', primary: true},
      {text: 'Yes', primary: false, action: openTrace},
      {text: 'Always trust', primary: false, action: trustAndOpenTrace},
    ],
  });
}

function sanitizePostedTrace(postedTrace: PostedTrace): PostedTrace {
  const result: PostedTrace = {
    title: sanitizeString(postedTrace.title),
    buffer: postedTrace.buffer,
    keepApiOpen: postedTrace.keepApiOpen,
  };
  if (postedTrace.url !== undefined) {
    result.url = sanitizeString(postedTrace.url);
  }
  return result;
}

function sanitizeString(str: string): string {
  return str.replace(/[^A-Za-z0-9.\-_#:/?=&;%+$ ]/g, ' ');
}

function isTraceViewerReady(): boolean {
  return !!(globals.getCurrentEngine()?.ready);
}

const _maxScrollToRangeAttempts = 20;
async function scrollToTimeRange(
    postedScrollToRange: PostedScrollToRange, maxAttempts?: number) {
  const ready = isTraceViewerReady();
  if (!ready) {
    if (maxAttempts === undefined) {
      maxAttempts = 0;
    }
    if (maxAttempts > _maxScrollToRangeAttempts) {
      console.warn('Could not scroll to time range. Trace viewer not ready.');
      return;
    }
    setTimeout(scrollToTimeRange, 200, postedScrollToRange, maxAttempts + 1);
  } else {
    focusHorizontalRange(
        postedScrollToRange.timeStart,
        postedScrollToRange.timeEnd,
        postedScrollToRange.viewPercentage);
  }
}

function isPostedScrollToRange(obj: unknown):
    obj is PostedScrollToRangeWrapped {
  const wrapped = obj as PostedScrollToRangeWrapped;
  if (wrapped.perfetto === undefined) {
    return false;
  }
  return wrapped.perfetto.timeStart !== undefined ||
      wrapped.perfetto.timeEnd !== undefined;
}

function isPostedTraceWrapped(obj: any): obj is PostedTraceWrapped {
  const wrapped = obj as PostedTraceWrapped;
  if (wrapped.perfetto === undefined) {
    return false;
  }
  return wrapped.perfetto.buffer !== undefined &&
      wrapped.perfetto.title !== undefined;
}
