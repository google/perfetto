// Copyright (C) 2018 The Android Open Source Project
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
import {inflate} from 'pako';
import {assertTrue} from '../base/logging';
import {isString} from '../base/object_utils';
import {showModal} from '../widgets/modal';
import {globals} from './globals';
import {utf8Decode} from '../base/string_utils';
import {convertToJson} from './trace_converter';

const CTRACE_HEADER = 'TRACE:\n';

async function isCtrace(file: File): Promise<boolean> {
  const fileName = file.name.toLowerCase();

  if (fileName.endsWith('.ctrace')) {
    return true;
  }

  // .ctrace files sometimes end with .txt. We can detect these via
  // the presence of TRACE: near the top of the file.
  if (fileName.endsWith('.txt')) {
    const header = await readText(file.slice(0, 128));
    if (header.includes(CTRACE_HEADER)) {
      return true;
    }
  }

  return false;
}

function readText(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      if (isString(reader.result)) {
        return resolve(reader.result);
      }
    };
    reader.onerror = (err) => {
      reject(err);
    };
    reader.readAsText(blob);
  });
}

export async function isLegacyTrace(file: File): Promise<boolean> {
  const fileName = file.name.toLowerCase();
  if (
    fileName.endsWith('.json') ||
    fileName.endsWith('.json.gz') ||
    fileName.endsWith('.zip') ||
    fileName.endsWith('.html')
  ) {
    return true;
  }

  if (await isCtrace(file)) {
    return true;
  }

  // Sometimes systrace formatted traces end with '.trace'. This is a
  // little generic to assume all such traces are systrace format though
  // so we read the beginning of the file and check to see if is has the
  // systrace header (several comment lines):
  if (fileName.endsWith('.trace')) {
    const header = await readText(file.slice(0, 512));
    const lines = header.split('\n');
    let commentCount = 0;
    for (const line of lines) {
      if (line.startsWith('#')) {
        commentCount++;
      }
    }
    if (commentCount > 5) {
      return true;
    }
  }

  return false;
}

export async function openFileWithLegacyTraceViewer(file: File) {
  const reader = new FileReader();
  reader.onload = () => {
    if (reader.result instanceof ArrayBuffer) {
      return openBufferWithLegacyTraceViewer(
        file.name,
        reader.result,
        reader.result.byteLength,
      );
    } else {
      const str = reader.result as string;
      return openBufferWithLegacyTraceViewer(file.name, str, str.length);
    }
  };
  reader.onerror = (err) => {
    console.error(err);
  };
  if (
    file.name.endsWith('.gz') ||
    file.name.endsWith('.zip') ||
    (await isCtrace(file))
  ) {
    reader.readAsArrayBuffer(file);
  } else {
    reader.readAsText(file);
  }
}

function openBufferWithLegacyTraceViewer(
  name: string,
  data: ArrayBuffer | string,
  size: number,
) {
  if (data instanceof ArrayBuffer) {
    assertTrue(size <= data.byteLength);
    if (size !== data.byteLength) {
      data = data.slice(0, size);
    }

    // Handle .ctrace files.
    const header = utf8Decode(data.slice(0, 128));
    if (header.includes(CTRACE_HEADER)) {
      const offset = header.indexOf(CTRACE_HEADER) + CTRACE_HEADER.length;
      data = inflate(new Uint8Array(data.slice(offset)), {to: 'string'});
    }
  }

  // The location.pathname mangling is to make this code work also when hosted
  // in a non-root sub-directory, for the case of CI artifacts.
  const catapultUrl = globals.root + 'assets/catapult_trace_viewer.html';
  const newWin = window.open(catapultUrl);
  if (newWin) {
    // Popup succeedeed.
    newWin.addEventListener('load', (e: Event) => {
      const doc = e.target as Document;
      const ctl = doc.querySelector('x-profiling-view') as TraceViewerAPI;
      ctl.setActiveTrace(name, data);
    });
    return;
  }

  // Popup blocker detected.
  showModal({
    title: 'Open trace in the legacy Catapult Trace Viewer',
    content: m(
      'div',
      m('div', 'You are seeing this interstitial because popups are blocked'),
      m('div', 'Enable popups to skip this dialog next time.'),
    ),
    buttons: [
      {
        text: 'Open legacy UI',
        primary: true,
        action: () => openBufferWithLegacyTraceViewer(name, data, size),
      },
    ],
  });
}

export function openInOldUIWithSizeCheck(trace: Blob) {
  // Perfetto traces smaller than 50mb can be safely opened in the legacy UI.
  if (trace.size < 1024 * 1024 * 50) {
    convertToJson(trace, openBufferWithLegacyTraceViewer);
    return;
  }

  // Give the user the option to truncate larger perfetto traces.
  const size = Math.round(trace.size / (1024 * 1024));
  showModal({
    title: 'Legacy UI may fail to open this trace',
    content: m(
      'div',
      m(
        'p',
        `This trace is ${size}mb, opening it in the legacy UI ` + `may fail.`,
      ),
      m(
        'p',
        'More options can be found at ',
        m(
          'a',
          {
            href: 'https://goto.google.com/opening-large-traces',
            target: '_blank',
          },
          'go/opening-large-traces',
        ),
        '.',
      ),
    ),
    buttons: [
      {
        text: 'Open full trace (not recommended)',
        action: () => convertToJson(trace, openBufferWithLegacyTraceViewer),
      },
      {
        text: 'Open beginning of trace',
        action: () =>
          convertToJson(
            trace,
            openBufferWithLegacyTraceViewer,
            /* truncate*/ 'start',
          ),
      },
      {
        text: 'Open end of trace',
        primary: true,
        action: () =>
          convertToJson(
            trace,
            openBufferWithLegacyTraceViewer,
            /* truncate*/ 'end',
          ),
      },
    ],
  });
  return;
}

// TraceViewer method that we wire up to trigger the file load.
interface TraceViewerAPI extends Element {
  setActiveTrace(name: string, data: ArrayBuffer | string): void;
}
