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
import {ErrorDetails} from '../base/logging';
import {EXTENSION_URL} from '../common/recordingV2/recording_utils';
import {GcsUploader} from '../common/gcs_uploader';
import {RECORDING_V2_FLAG} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {VERSION} from '../gen/perfetto_version';
import {getCurrentModalKey, showModal} from '../widgets/modal';
import {globals} from './globals';
import {AppImpl} from '../core/app_impl';
import {Router} from '../core/router';

const MODAL_KEY = 'crash_modal';

// Never show more than one dialog per 10s.
const MIN_REPORT_PERIOD_MS = 10000;
let timeLastReport = 0;

export function maybeShowErrorDialog(err: ErrorDetails) {
  const now = performance.now();

  // Here we rely on the exception message from onCannotGrowMemory function
  if (
    err.message.includes('Cannot enlarge memory') ||
    err.stack.some((entry) => entry.name.includes('OutOfMemoryHandler')) ||
    err.stack.some((entry) => entry.name.includes('_emscripten_resize_heap')) ||
    err.stack.some((entry) => entry.name.includes('sbrk')) ||
    /^out of memory$/m.exec(err.message)
  ) {
    showOutOfMemoryDialog();
    // Refresh timeLastReport to prevent a different error showing a dialog
    timeLastReport = now;
    return;
  }

  if (!RECORDING_V2_FLAG.get()) {
    if (err.message.includes('Unable to claim interface')) {
      showWebUSBError();
      timeLastReport = now;
      return;
    }

    if (
      err.message.includes('A transfer error has occurred') ||
      err.message.includes('The device was disconnected') ||
      err.message.includes('The transfer was cancelled')
    ) {
      showConnectionLostError();
      timeLastReport = now;
      return;
    }
  }

  if (err.message.includes('(ERR:fmt)')) {
    showUnknownFileError();
    return;
  }

  if (err.message.includes('(ERR:rpc_seq)')) {
    showRpcSequencingError();
    return;
  }

  if (err.message.includes('(ERR:ws)')) {
    showWebsocketConnectionIssue(err.message);
    return;
  }

  // This is only for older version of the UI and for ease of tracking across
  // cherry-picks. Newer versions don't have this exception anymore.
  if (err.message.includes('State hash does not match')) {
    showNewerStateError();
    return;
  }

  if (timeLastReport > 0 && now - timeLastReport <= MIN_REPORT_PERIOD_MS) {
    console.log('Suppressing crash dialog, last error notified too soon.');
    return;
  }
  timeLastReport = now;

  // If we are already showing a crash dialog, don't overwrite it with a newer
  // crash. Usually the first crash matters, the rest avalanching effects.
  if (getCurrentModalKey() === MODAL_KEY) {
    return;
  }

  showModal({
    key: MODAL_KEY,
    title: 'Oops, something went wrong. Please file a bug.',
    content: () => m(ErrorDialogComponent, err),
  });
}

class ErrorDialogComponent implements m.ClassComponent<ErrorDetails> {
  private traceState:
    | 'NOT_AVAILABLE'
    | 'NOT_UPLOADED'
    | 'UPLOADING'
    | 'UPLOADED';
  private traceType: string = 'No trace loaded';
  private traceData?: ArrayBuffer | File;
  private traceUrl?: string;
  private attachTrace = false;
  private uploadStatus = '';
  private userDescription = '';
  private errorMessage = '';
  private uploader?: GcsUploader;

  constructor() {
    this.traceState = 'NOT_AVAILABLE';
    const traceSource = AppImpl.instance.trace?.traceInfo.source;
    if (traceSource === undefined) return;
    this.traceType = traceSource.type;
    // If the trace is either already uploaded, or comes from a postmessage+url
    // we don't need any re-upload.
    if ('url' in traceSource && traceSource.url !== undefined) {
      this.traceUrl = traceSource.url;
      this.traceState = 'UPLOADED';
      // The trace is already uploaded, so assume the user is fine attaching to
      // the bugreport (this make the checkbox ticked by default).
      this.attachTrace = true;
      return;
    }

    // If the user is not a googler, don't even offer the option to upload it.
    if (!globals.isInternalUser) return;

    if (traceSource.type === 'FILE') {
      this.traceState = 'NOT_UPLOADED';
      this.traceData = traceSource.file;
      // this.traceSize = this.traceData.size;
    } else if (traceSource.type === 'ARRAY_BUFFER') {
      this.traceData = traceSource.buffer;
      // this.traceSize = this.traceData.byteLength;
    } else {
      return; // Can't upload HTTP+RPC.
    }
    this.traceState = 'NOT_UPLOADED';
  }

  view(vnode: m.Vnode<ErrorDetails>) {
    const err = vnode.attrs;
    let msg = `UI: ${location.protocol}//${location.host}/${VERSION}\n\n`;

    // Append the trace stack.
    msg += `${err.message}\n`;
    for (const entry of err.stack) {
      msg += ` - ${entry.name} (${entry.location})\n`;
    }
    msg += '\n';

    // Append the trace URL.
    if (this.attachTrace && this.traceUrl) {
      msg += `Trace: ${this.traceUrl}\n`;
    } else if (this.attachTrace && this.traceState === 'UPLOADING') {
      msg += `Trace: uploading...\n`;
    } else {
      msg += `Trace: not available (${this.traceType}). Provide repro steps.\n`;
    }
    msg += `UA: ${navigator.userAgent}\n`;
    msg += `Referrer: ${document.referrer}\n`;
    this.errorMessage = msg;

    let shareTraceSection: m.Vnode | null = null;
    if (this.traceState !== 'NOT_AVAILABLE') {
      shareTraceSection = m(
        'div',
        m(
          'label',
          m(`input[type=checkbox]`, {
            checked: this.attachTrace,
            oninput: (ev: InputEvent) => {
              const checked = (ev.target as HTMLInputElement).checked;
              this.onUploadCheckboxChange(checked);
            },
          }),
          this.traceState === 'UPLOADING'
            ? `Uploading trace... ${this.uploadStatus}`
            : 'Tick to share the current trace and help debugging',
        ), // m('label')
        m(
          'div.modal-small',
          `This will upload the trace and attach a link to the bug.
          You may leave it unchecked and attach the trace manually to the bug
          if preferred.`,
        ),
      );
    } // if (this.traceState !== 'NOT_AVAILABLE')

    return [
      m(
        'div',
        m('.modal-logs', msg),
        m(
          'span',
          `Please provide any additional details describing
        how the crash occurred:`,
        ),
        m('textarea.modal-textarea', {
          rows: 3,
          maxlength: 1000,
          oninput: (ev: InputEvent) => {
            this.userDescription = (ev.target as HTMLTextAreaElement).value;
          },
          onkeydown: (e: Event) => e.stopPropagation(),
          onkeyup: (e: Event) => e.stopPropagation(),
        }),
        shareTraceSection,
      ),
      m(
        'footer',
        m(
          'button.modal-btn.modal-btn-primary',
          {onclick: () => this.fileBug(err)},
          'File a bug (Googlers only)',
        ),
      ),
    ];
  }

  private onUploadCheckboxChange(checked: boolean) {
    raf.scheduleFullRedraw();
    this.attachTrace = checked;

    if (
      checked &&
      this.traceData !== undefined &&
      this.traceState === 'NOT_UPLOADED'
    ) {
      this.traceState = 'UPLOADING';
      this.uploadStatus = '';
      const uploader = new GcsUploader(this.traceData, {
        onProgress: () => {
          raf.scheduleFullRedraw();
          this.uploadStatus = uploader.getEtaString();
          if (uploader.state === 'UPLOADED') {
            this.traceState = 'UPLOADED';
            this.traceUrl = uploader.uploadedUrl;
          } else if (uploader.state === 'ERROR') {
            this.traceState = 'NOT_UPLOADED';
            this.uploadStatus = uploader.error;
          }
        },
      });
      this.uploader = uploader;
    } else if (!checked && this.uploader) {
      this.uploader.abort();
    }
  }

  private fileBug(err: ErrorDetails) {
    const errTitle = err.message.split('\n', 1)[0].substring(0, 80);
    let url = 'https://goto.google.com/perfetto-ui-bug';
    url += '?title=' + encodeURIComponent(`UI Error: ${errTitle}`);
    url += '&description=';
    if (this.userDescription !== '') {
      url += encodeURIComponent(
        'User description:\n' + this.userDescription + '\n\n',
      );
    }
    url += encodeURIComponent(this.errorMessage);
    // 8kb is common limit on request size so restrict links to that long:
    url = url.substring(0, 8000);
    window.open(url, '_blank');
  }
}

function showOutOfMemoryDialog() {
  const url =
    'https://perfetto.dev/docs/quickstart/trace-analysis#get-trace-processor';

  const tpCmd =
    'curl -LO https://get.perfetto.dev/trace_processor\n' +
    'chmod +x ./trace_processor\n' +
    'trace_processor --httpd /path/to/trace.pftrace\n' +
    '# Reload the UI, it will prompt to use the HTTP+RPC interface';
  showModal({
    title: 'Oops! Your WASM trace processor ran out of memory',
    content: m(
      'div',
      m(
        'span',
        'The in-memory representation of the trace is too big ' +
          'for the browser memory limits (typically 2GB per tab).',
      ),
      m('br'),
      m(
        'span',
        'You can work around this problem by using the trace_processor ' +
          'native binary as an accelerator for the UI as follows:',
      ),
      m('br'),
      m('br'),
      m('.modal-bash', tpCmd),
      m('br'),
      m('span', 'For details see '),
      m('a', {href: url, target: '_blank'}, url),
    ),
  });
}

function showUnknownFileError() {
  showModal({
    title: 'Cannot open this file',
    content: m(
      'div',
      m(
        'p',
        "The file opened doesn't look like a Perfetto trace or any " +
          'other format recognized by the Perfetto TraceProcessor.',
      ),
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
  });
}

function showWebUSBError() {
  showModal({
    title: 'A WebUSB error occurred',
    content: m(
      'div',
      m(
        'span',
        `Is adb already running on the host? Run this command and
      try again.`,
      ),
      m('br'),
      m('.modal-bash', '> adb kill-server'),
      m('br'),
      m('span', 'For details see '),
      m('a', {href: 'http://b/159048331', target: '_blank'}, 'b/159048331'),
    ),
  });
}

export function showWebUSBErrorV2() {
  showModal({
    title: 'A WebUSB error occurred',
    content: m(
      'div',
      m(
        'span',
        `Is adb already running on the host? Run this command and
      try again.`,
      ),
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
      m(
        'span',
        "If after running 'adb kill-server', you don't see " +
          "a 'Start Recording' button on the page and you don't see " +
          "'Allow USB debugging' on the device, " +
          'you will need to reload this page.',
      ),
      m('br'),
      m('br'),
      m('span', 'For details see '),
      m('a', {href: 'http://b/159048331', target: '_blank'}, 'b/159048331'),
    ),
  });
}

export function showConnectionLostError(): void {
  showModal({
    title: 'Connection with the ADB device lost',
    content: m(
      'div',
      m('span', `Please connect the device again to restart the recording.`),
      m('br'),
    ),
  });
}

export function showAllowUSBDebugging(): void {
  showModal({
    title: 'Could not connect to the device',
    content: m(
      'div',
      m('span', 'Please allow USB debugging on the device.'),
      m('br'),
    ),
  });
}

export function showNoDeviceSelected(): void {
  showModal({
    title: 'No device was selected for recording',
    content: m(
      'div',
      m(
        'span',
        `If you want to connect to an ADB device,
           please select it from the list.`,
      ),
      m('br'),
    ),
  });
}

export function showExtensionNotInstalled(): void {
  showModal({
    title: 'Perfetto Chrome extension not installed',
    content: m(
      'div',
      m(
        '.note',
        `To trace Chrome from the Perfetto UI, you need to install our `,
        m('a', {href: EXTENSION_URL, target: '_blank'}, 'Chrome extension'),
        ' and then reload this page.',
      ),
      m('br'),
    ),
  });
}

export function showWebsocketConnectionIssue(message: string): void {
  showModal({
    title: 'Unable to connect to the device via websocket',
    content: m(
      'div',
      m('div', 'trace_processor_shell --httpd is unreachable or crashed.'),
      m('pre', message),
    ),
  });
}

export function showIssueParsingTheTracedResponse(message: string): void {
  showModal({
    title:
      'A problem was encountered while connecting to' +
      ' the Perfetto tracing service',
    content: m('div', m('span', message), m('br')),
  });
}

export function showFailedToPushBinary(message: string): void {
  showModal({
    title: 'Failed to push a binary to the device',
    content: m(
      'div',
      m(
        'span',
        'This can happen if your Android device has an OS version lower ' +
          'than Q. Perfetto tried to push the latest version of its ' +
          'embedded binary but failed.',
      ),
      m('br'),
      m('br'),
      m('span', 'Error message:'),
      m('br'),
      m('span', message),
    ),
  });
}

function showRpcSequencingError() {
  showModal({
    title: 'A TraceProcessor RPC error occurred',
    content: m(
      'div',
      m('p', 'The trace processor RPC sequence ID was broken'),
      m(
        'p',
        `This can happen when using a HTTP trace processor instance and
either accidentally sharing this between multiple tabs or
restarting the trace processor while still in use by UI.`,
      ),
      m(
        'p',
        `Please refresh this tab and ensure that trace processor is used
at most one tab at a time.`,
      ),
    ),
  });
}

function showNewerStateError() {
  showModal({
    title: 'Cannot deserialize the permalink',
    content: m(
      'div',
      m('p', "The state hash doesn't match."),
      m(
        'p',
        'This usually happens when the permalink is generated by a version ' +
          'the UI that is newer than the current version, e.g., when a ' +
          'colleague created the permalink using the Canary or Autopush ' +
          'channel and you are trying to open it using Stable channel.',
      ),
      m(
        'p',
        'Try switching to Canary or Autopush channel from the Flags page ' +
          ' and try again.',
      ),
    ),
    buttons: [
      {
        text: 'Take me to the flags page',
        primary: true,
        action: () => Router.navigate('#!/flags/releaseChannel'),
      },
    ],
  });
}
