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

import {assertExists} from '../base/logging';
import {RECORDING_V2_FLAG} from '../common/feature_flags';
import {EXTENSION_URL} from '../common/recordingV2/recording_utils';
import {TraceUrlSource} from '../common/state';
import {saveTrace} from '../common/upload_utils';

import {globals} from './globals';
import {showModal} from './modal';
import {isShareable} from './trace_attrs';

// Never show more than one dialog per minute.
const MIN_REPORT_PERIOD_MS = 60000;
let timeLastReport = 0;

// Keeps the last ERR_QUEUE_MAX_LEN errors while the dialog is throttled.
const queuedErrors = new Array<string>();
const ERR_QUEUE_MAX_LEN = 10;

export function maybeShowErrorDialog(errLog: string) {
  globals.logging.logError(errLog);
  const now = performance.now();

  // Here we rely on the exception message from onCannotGrowMemory function
  if (errLog.includes('Cannot enlarge memory')) {
    showOutOfMemoryDialog();
    // Refresh timeLastReport to prevent a different error showing a dialog
    timeLastReport = now;
    return;
  }

  if (!RECORDING_V2_FLAG.get()) {
    if (errLog.includes('Unable to claim interface')) {
      showWebUSBError();
      timeLastReport = now;
      return;
    }

    if (errLog.includes('A transfer error has occurred') ||
        errLog.includes('The device was disconnected') ||
        errLog.includes('The transfer was cancelled')) {
      showConnectionLostError();
      timeLastReport = now;
      return;
    }
  }

  if (errLog.includes('(ERR:fmt)')) {
    showUnknownFileError();
    return;
  }

  if (errLog.includes('(ERR:rpc_seq)')) {
    showRpcSequencingError();
    return;
  }

  if (timeLastReport > 0 && now - timeLastReport <= MIN_REPORT_PERIOD_MS) {
    queuedErrors.unshift(errLog);
    if (queuedErrors.length > ERR_QUEUE_MAX_LEN) queuedErrors.pop();
    console.log('Suppressing crash dialog, last error notified too soon.');
    return;
  }
  timeLastReport = now;

  // Append queued errors.
  while (queuedErrors.length > 0) {
    const queuedErr = queuedErrors.shift();
    errLog += `\n\n---------------------------------------\n${queuedErr}`;
  }

  const errTitle = errLog.split('\n', 1)[0].substr(0, 80);
  const userDescription = '';
  let checked = false;
  const engine = globals.getCurrentEngine();

  const shareTraceSection: m.Vnode[] = [];
  if (isShareable() && !urlExists()) {
    shareTraceSection.push(
        m(`input[type=checkbox]`, {
          checked,
          oninput: (ev: InputEvent) => {
            checked = (ev.target as HTMLInputElement).checked;
            if (checked && engine && engine.source.type === 'FILE') {
              saveTrace(engine.source.file).then((url) => {
                const errMessage = createErrorMessage(errLog, checked, url);
                renderModal(
                    errTitle, errMessage, userDescription, shareTraceSection);
                return;
              });
            }
            const errMessage = createErrorMessage(errLog, checked);
            renderModal(
                errTitle, errMessage, userDescription, shareTraceSection);
          },
        }),
        m('span', `Check this box to share the current trace for debugging
     purposes.`),
        m('div.modal-small',
          `This will create a permalink to this trace, you may
     leave it unchecked and attach the trace manually
     to the bug if preferred.`));
  }
  renderModal(
      errTitle,
      createErrorMessage(errLog, checked),
      userDescription,
      shareTraceSection);
}

function renderModal(
    errTitle: string,
    errMessage: string,
    userDescription: string,
    shareTraceSection: m.Vnode[]) {
  showModal({
    title: 'Oops, something went wrong. Please file a bug.',
    content:
        m('div',
          m('.modal-logs', errMessage),
          m('span', `Please provide any additional details describing
           how the crash occurred:`),
          m('textarea.modal-textarea', {
            rows: 3,
            maxlength: 1000,
            oninput: (ev: InputEvent) => {
              userDescription = (ev.target as HTMLTextAreaElement).value;
            },
            onkeydown: (e: Event) => {
              e.stopPropagation();
            },
            onkeyup: (e: Event) => {
              e.stopPropagation();
            },
          }),
          shareTraceSection),
    buttons: [
      {
        text: 'File a bug (Googlers only)',
        primary: true,
        id: 'file_bug',
        action: () => {
          window.open(
              createLink(errTitle, errMessage, userDescription), '_blank');
        },
      },
    ],
  });
}

// If there is a trace URL to share, we don't have to show the upload checkbox.
function urlExists() {
  const engine = globals.getCurrentEngine();
  return engine !== undefined &&
      (engine.source.type === 'ARRAY_BUFFER' || engine.source.type === 'URL') &&
      engine.source.url !== undefined;
}

function createErrorMessage(errLog: string, checked: boolean, url?: string) {
  let errMessage = '';
  const engine = globals.getCurrentEngine();
  if (checked && url !== undefined) {
    errMessage += `Trace: ${url}`;
  } else if (urlExists()) {
    errMessage +=
        `Trace: ${(assertExists(engine).source as TraceUrlSource).url}`;
  } else {
    errMessage += 'To assist with debugging please attach or link to the ' +
        'trace you were viewing.';
  }
  return errMessage + '\n\n' +
      'Viewed on: ' + self.location.origin + '\n\n' + errLog;
}

function createLink(
    errTitle: string, errMessage: string, userDescription: string): string {
  let link = 'https://goto.google.com/perfetto-ui-bug';
  link += '?title=' + encodeURIComponent(`UI Error: ${errTitle}`);
  link += '&description=';
  if (userDescription !== '') {
    link +=
        encodeURIComponent('User description:\n' + userDescription + '\n\n');
  }
  link += encodeURIComponent(errMessage);
  // 8kb is common limit on request size so restrict links to that long:
  return link.substr(0, 8000);
}

function showOutOfMemoryDialog() {
  const url =
      'https://perfetto.dev/docs/quickstart/trace-analysis#get-trace-processor';

  const tpCmd = 'curl -LO https://get.perfetto.dev/trace_processor\n' +
      'chmod +x ./trace_processor\n' +
      'trace_processor --httpd /path/to/trace.pftrace\n' +
      '# Reload the UI, it will prompt to use the HTTP+RPC interface';
  showModal({
    title: 'Oops! Your WASM trace processor ran out of memory',
    content: m(
        'div',
        m('span',
          'The in-memory representation of the trace is too big ' +
              'for the browser memory limits (typically 2GB per tab).'),
        m('br'),
        m('span',
          'You can work around this problem by using the trace_processor ' +
              'native binary as an accelerator for the UI as follows:'),
        m('br'),
        m('br'),
        m('.modal-bash', tpCmd),
        m('br'),
        m('span', 'For details see '),
        m('a', {href: url, target: '_blank'}, url),
        ),
    buttons: [],
  });
}

function showUnknownFileError() {
  showModal({
    title: 'Cannot open this file',
    content: m(
        'div',
        m('p',
          'The file opened doesn\'t look like a Perfetto trace or any ' +
              'other format recognized by the Perfetto TraceProcessor.'),
        m('p', 'Formats supported:'),
        m(
            'ul',
            m('li', 'Perfetto protobuf trace'),
            m('li', 'chrome://tracing JSON'),
            m('li', 'Android systrace'),
            m('li', 'Fuchsia trace'),
            m('li', 'Ninja build log'),
            ),
        ),
    buttons: [],
  });
}

function showWebUSBError() {
  showModal({
    title: 'A WebUSB error occurred',
    content: m(
        'div',
        m('span', `Is adb already running on the host? Run this command and
      try again.`),
        m('br'),
        m('.modal-bash', '> adb kill-server'),
        m('br'),
        m('span', 'For details see '),
        m('a', {href: 'http://b/159048331', target: '_blank'}, 'b/159048331'),
        ),
    buttons: [],
  });
}

export function showWebUSBErrorV2() {
  showModal({
    title: 'A WebUSB error occurred',
    content: m(
        'div',
        m('span', `Is adb already running on the host? Run this command and
      try again.`),
        m('br'),
        m('.modal-bash', '> adb kill-server'),
        m('br'),
        // The statement below covers the following edge case:
        // 1. 'adb server' is running on the device.
        // 2. The user selects the new Android target, so we try to fetch the
        // OS version and do QSS.
        // 3. The error modal is shown.
        // 4. The user runs 'adb kill-server'.
        // At this point we don't have a trigger to try fetching the OS version
        // + QSS again. Therefore, the user will need to refresh the page.
        m('span',
          'If after running \'adb kill-server\', you don\'t see ' +
              'a \'Start Recording\' button on the page and you don\'t see ' +
              '\'Allow USB debugging\' on the device, ' +
              'you will need to reload this page.'),
        m('br'),
        m('br'),
        m('span', 'For details see '),
        m('a', {href: 'http://b/159048331', target: '_blank'}, 'b/159048331'),
        ),
    buttons: [],
  });
}

export function showConnectionLostError(): void {
  showModal({
    title: 'Connection with the ADB device lost',
    content: m(
        'div',
        m('span', `Please connect the device again to restart the recording.`),
        m('br')),
    buttons: [],
  });
}

export function showAllowUSBDebugging(): void {
  showModal({
    title: 'Could not connect to the device',
    content: m(
        'div', m('span', 'Please allow USB debugging on the device.'), m('br')),
    buttons: [],
  });
}

export function showNoDeviceSelected(): void {
  showModal({
    title: 'No device was selected for recording',
    content:
        m('div',
          m('span', `If you want to connect to an ADB device,
           please select it from the list.`),
          m('br')),
    buttons: [],
  });
}

export function showExtensionNotInstalled(): void {
  showModal({
    title: 'Perfetto Chrome extension not installed',
    content:
        m('div',
          m('.note',
            `To trace Chrome from the Perfetto UI, you need to install our `,
            m('a', {href: EXTENSION_URL, target: '_blank'}, 'Chrome extension'),
            ' and then reload this page.'),
          m('br')),
    buttons: [],
  });
}

export function showWebsocketConnectionIssue(message: string): void {
  showModal({
    title: 'Unable to connect to the device via websocket',
    content: m('div', m('span', message), m('br')),
    buttons: [],
  });
}

export function showIssueParsingTheTracedResponse(message: string): void {
  showModal({
    title: 'A problem was encountered while connecting to' +
        ' the Perfetto tracing service',
    content: m('div', m('span', message), m('br')),
    buttons: [],
  });
}

export function showFailedToPushBinary(message: string): void {
  showModal({
    title: 'Failed to push a binary to the device',
    content:
        m('div',
          m('span',
            'This can happen if your Android device has an OS version lower ' +
                'than Q. Perfetto tried to push the latest version of its ' +
                'embedded binary but failed.'),
          m('br'),
          m('br'),
          m('span', 'Error message:'),
          m('br'),
          m('span', message)),
    buttons: [],
  });
}

function showRpcSequencingError() {
  showModal({
    title: 'A TraceProcessor RPC error occurred',
    content: m(
        'div',
        m('p', 'The trace processor RPC sequence ID was broken'),
        m('p', `This can happen when using a HTTP trace processor instance and
either accidentally sharing this between multiple tabs or
restarting the trace processor while still in use by UI.`),
        m('p', `Please refresh this tab and ensure that trace processor is used
at most one tab at a time.`),
        ),
    buttons: [],
  });
}
