// Copyright (C) 2026 The Android Open Source Project
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
import {
  disableMetatracingAndGetTrace,
  enableMetatracing,
  getEnabledMetatracingCategories,
  isMetatracingEnabled,
} from '../core/metatracing';
import {Engine} from '../trace_processor/engine';
import {AppImpl} from '../core/app_impl';
import {Trace} from '../public/trace';
import {TraceImpl} from '../core/trace_impl';
import {download, downloadUrl} from '../base/download_utils';
import {
  convertTraceToJsonAndDownload,
  convertTraceToSystraceAndDownload,
} from './trace_converter';
import {openInOldUIWithSizeCheck} from './legacy_trace_viewer';
import {showModal} from '../widgets/modal';
import {assertExists} from '../base/assert';

const TRACE_SUFFIX = '.perfetto-trace';

export async function openCurrentTraceWithOldUI(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent(
    'Trace Actions',
    'Open current trace in legacy UI',
  );
  const file = await trace.getTraceFile();
  await openInOldUIWithSizeCheck(file);
}

export async function convertTraceToSystrace(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Convert to .systrace');
  const file = await trace.getTraceFile();
  await convertTraceToSystraceAndDownload(file);
}

export async function convertTraceToJson(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Convert to .json');
  const file = await trace.getTraceFile();
  await convertTraceToJsonAndDownload(file);
}

export function downloadTrace(trace: TraceImpl) {
  if (!trace.traceInfo.downloadable) return;
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Download trace');

  const src = trace.traceInfo.source;
  const filePickerAcceptTypes = [
    {
      description: 'Perfetto trace',
      accept: {'*/*': ['.pftrace', '.gz']},
    },
  ];
  if (src.type === 'URL') {
    const fileName = src.url.split('/').slice(-1)[0];
    downloadUrl({url: src.url, fileName});
  } else if (src.type === 'ARRAY_BUFFER') {
    const blob = new Blob([src.buffer], {type: 'application/octet-stream'});
    const fileName = src.fileName ?? `trace${TRACE_SUFFIX}`;
    download({
      content: blob,
      fileName,
      filePicker: {
        types: filePickerAcceptTypes,
      },
    });
  } else if (src.type === 'FILE') {
    download({
      content: src.file,
      fileName: src.file.name,
      filePicker: {
        types: filePickerAcceptTypes,
      },
    });
  } else {
    throw new Error(`Download from ${JSON.stringify(src)} is not supported`);
  }
}

function recordMetatrace(engine: Engine) {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Record metatrace');

  const highPrecisionTimersAvailable =
    window.crossOriginIsolated || engine.mode === 'HTTP_RPC';
  if (!highPrecisionTimersAvailable) {
    const PROMPT = `High-precision timers are not available to WASM trace processor yet.

Modern browsers restrict high-precision timers to cross-origin-isolated pages.
As Perfetto UI needs to open traces via postMessage, it can't be cross-origin
isolated until browsers ship support for
'Cross-origin-opener-policy: restrict-properties'.

Do you still want to record a metatrace?
Note that events under timer precision (1ms) will dropped.
Alternatively, connect to a trace_processor_shell --httpd instance.
`;
    showModal({
      title: `Trace processor doesn't have high-precision timers`,
      content: m('.pf-modal-pre', PROMPT),
      buttons: [
        {
          text: 'YES, record metatrace',
          primary: true,
          action: () => {
            enableMetatracing();
            engine.enableMetatrace(
              assertExists(getEnabledMetatracingCategories()),
            );
          },
        },
        {
          text: 'NO, cancel',
        },
      ],
    });
  } else {
    enableMetatracing();
    engine.enableMetatrace(assertExists(getEnabledMetatracingCategories()));
  }
}

export async function toggleMetatrace(e: Engine) {
  return isMetatracingEnabled() ? finaliseMetatrace(e) : recordMetatrace(e);
}

async function finaliseMetatrace(engine: Engine) {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Finalise metatrace');

  const jsEvents = disableMetatracingAndGetTrace();

  const result = await engine.stopAndGetMetatrace();
  if (result.error.length !== 0) {
    throw new Error(`Failed to read metatrace: ${result.error}`);
  }

  download({
    fileName: 'metatrace',
    content: new Blob([result.metatrace, jsEvents]),
  });
}
