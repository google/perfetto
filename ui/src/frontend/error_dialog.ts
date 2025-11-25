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
import {GcsUploader} from '../base/gcs_uploader';
import {raf} from '../core/raf_scheduler';
import {VERSION} from '../gen/perfetto_version';
import {getCurrentModalKey, showModal} from '../widgets/modal';
import {AppImpl} from '../core/app_impl';
import {Router} from '../core/router';
import {Button, ButtonVariant} from '../widgets/button';
import {Intent} from '../widgets/common';
import {Checkbox} from '../widgets/checkbox';
import {Anchor} from '../widgets/anchor';
import {Icons} from '../base/semantic_icons';
import {mapStackTraceWithMinifiedSourceMap} from '../base/source_map_utils';

const MODAL_KEY = 'crash_modal';

// Never show more than one dialog per 10s.
const MIN_REPORT_PERIOD_MS = 10000;
let timeLastReport = 0;

export function maybeShowErrorDialog(err: ErrorDetails) {
  const now = performance.now();

  // Here we rely on the exception message from onCannotGrowMemory function
  if (
    err.message.includes('Cannot enlarge memory') ||
    err.stack.some((entry) => entry.name.includes('base::AlignedAlloc')) ||
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

  if (err.message.includes('Unable to claim interface')) {
    showWebUSBError();
    timeLastReport = now;
    return;
  }

  if (err.message.includes('ABT: Got no attachments from extension')) {
    showABTError();
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

  err.stack = mapStackTraceWithMinifiedSourceMap(err.stack);

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
    if (!AppImpl.instance.isInternalUser) return;

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
        m(Checkbox, {
          checked: this.attachTrace,
          oninput: (ev: InputEvent) => {
            const checked = (ev.target as HTMLInputElement).checked;
            this.onUploadCheckboxChange(checked);
          },
          label:
            this.traceState === 'UPLOADING'
              ? `Uploading trace... ${this.uploadStatus}`
              : 'Tick to share the current trace and help debugging',
        }),
        m(
          'div.pf-modal-small',
          `This will upload the trace and attach a link to the bug.
          You may leave it unchecked and attach the trace manually to the bug
          if preferred.`,
        ),
      );
    } // if (this.traceState !== 'NOT_AVAILABLE')

    return [
      m(
        'div',
        m('.pf-modal-logs', msg),
        m(
          'span',
          `Please provide any additional details describing
        how the crash occurred:`,
        ),
        m('textarea.pf-modal-textarea', {
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
        m(Button, {
          onclick: () => this.fileBug(err),
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          label: 'File a bug (Googlers only)',
        }),
      ),
    ];
  }

  private onUploadCheckboxChange(checked: boolean) {
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
    './trace_processor --httpd /path/to/trace.pftrace\n' +
    '# Reload the UI, it will prompt to use the HTTP+RPC interface';
  showModal({
    title: 'Oops! Your WASM trace processor ran out of memory',
    content: m(
      'div',
      m(
        'span',
        'The in-memory representation of the trace is too big ' +
          'for the browser memory limits.',
      ),
      m('br'),
      m(
        'span',
        'You can work around this problem by using the trace_processor ' +
          'native binary as an accelerator for the UI as follows:',
      ),
      m('br'),
      m('br'),
      m('.pf-modal-bash', tpCmd),
      m('br'),
      m('span', 'For details see '),
      m(Anchor, {href: url, target: '_blank', icon: Icons.ExternalLink}, url),
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
        m('li', 'pprof'),
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
        `Cannot access the USB interface for ADB. This can happen when:`,
      ),
      m('br'),
      m('br'),
      m(
        'ul',
        m('li', 'Another tool is already using ADB (e.g., chrome://inspect)'),
        m('li', 'ADB server is running on the host machine'),
        m('li', 'Another profiling tool has exclusive access to the device'),
      ),
      m('br'),
      m('span', 'Try the following solutions:'),
      m('br'),
      m('br'),
      m(
        'ol',
        m('li', 'Close chrome://inspect or other debugging tools'),
        m('li', 'Run the command below to kill the ADB server:'),
      ),
      m('.pf-modal-bash', '> adb kill-server'),
      m('br'),
      m('span', '3. Disconnect and reconnect your device'),
      m('br'),
      m('br'),
      m(
        'span',
        'Note: Perfetto and chrome://inspect cannot be used simultaneously as they both require exclusive access to the USB ADB interface.',
      ),
    ),
  });
}

function showABTError() {
  showModal({
    title: 'An ABT error occurred',
    content: m(
      'div',
      m(
        'span',
        `The Android Bug Tool (ABT) Chrome extension did not pass a valid file.`,
      ),
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

function showWebsocketConnectionIssue(message: string): void {
  showModal({
    title: 'Unable to connect to the device via websocket',
    content: m(
      'div',
      m('div', 'trace_processor_shell --httpd is unreachable or crashed.'),
      m('pre', message),
    ),
  });
}

function showConnectionLostError(): void {
  showModal({
    title: 'Connection with the ADB device lost',
    content: m(
      'div',
      m('span', `Please connect the device again to restart the recording.`),
      m('br'),
    ),
  });
}
