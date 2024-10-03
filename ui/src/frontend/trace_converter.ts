// Copyright (C) 2021 The Android Open Source Project
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

import {download} from '../base/clipboard';
import {ErrorDetails} from '../base/logging';
import {utf8Decode} from '../base/string_utils';
import {time} from '../base/time';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
import {AppImpl} from '../core/app_impl';
import {maybeShowErrorDialog} from './error_dialog';
import {globals} from './globals';

type Args =
  | UpdateStatusArgs
  | UpdateJobStatusArgs
  | DownloadFileArgs
  | OpenTraceInLegacyArgs
  | ErrorArgs;

interface UpdateStatusArgs {
  kind: 'updateStatus';
  status: string;
}

interface UpdateJobStatusArgs {
  kind: 'updateJobStatus';
  name: ConversionJobName;
  status: ConversionJobStatus;
}

interface DownloadFileArgs {
  kind: 'downloadFile';
  buffer: Uint8Array;
  name: string;
}

interface OpenTraceInLegacyArgs {
  kind: 'openTraceInLegacy';
  buffer: Uint8Array;
}

interface ErrorArgs {
  kind: 'error';
  error: ErrorDetails;
}

type OpenTraceInLegacyCallback = (
  name: string,
  data: ArrayBuffer | string,
  size: number,
) => void;

function makeWorkerAndPost(
  msg: unknown,
  openTraceInLegacy?: OpenTraceInLegacyCallback,
) {
  function handleOnMessage(msg: MessageEvent): void {
    const args: Args = msg.data;
    if (args.kind === 'updateStatus') {
      AppImpl.instance.omnibox.showStatusMessage(args.status);
    } else if (args.kind === 'updateJobStatus') {
      globals.setConversionJobStatus(args.name, args.status);
    } else if (args.kind === 'downloadFile') {
      download(new File([new Blob([args.buffer])], args.name));
    } else if (args.kind === 'openTraceInLegacy') {
      const str = utf8Decode(args.buffer);
      openTraceInLegacy?.('trace.json', str, 0);
    } else if (args.kind === 'error') {
      maybeShowErrorDialog(args.error);
    } else {
      throw new Error(`Unhandled message ${JSON.stringify(args)}`);
    }
  }

  const worker = new Worker(globals.root + 'traceconv_bundle.js');
  worker.onmessage = handleOnMessage;
  worker.postMessage(msg);
}

export function convertTraceToJsonAndDownload(trace: Blob) {
  makeWorkerAndPost({
    kind: 'ConvertTraceAndDownload',
    trace,
    format: 'json',
  });
}

export function convertTraceToSystraceAndDownload(trace: Blob) {
  makeWorkerAndPost({
    kind: 'ConvertTraceAndDownload',
    trace,
    format: 'systrace',
  });
}

export function convertToJson(
  trace: Blob,
  openTraceInLegacy: OpenTraceInLegacyCallback,
  truncate?: 'start' | 'end',
) {
  makeWorkerAndPost(
    {
      kind: 'ConvertTraceAndOpenInLegacy',
      trace,
      truncate,
    },
    openTraceInLegacy,
  );
}

export function convertTraceToPprofAndDownload(
  trace: Blob,
  pid: number,
  ts: time,
) {
  makeWorkerAndPost({
    kind: 'ConvertTraceToPprof',
    trace,
    pid,
    ts,
  });
}
