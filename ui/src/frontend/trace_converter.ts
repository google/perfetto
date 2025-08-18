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

import {assetSrc} from '../base/assets';
import {defer} from '../base/deferred';
import {download} from '../base/download_utils';
import {ErrorDetails} from '../base/logging';
import {utf8Decode} from '../base/string_utils';
import {time} from '../base/time';
import {AppImpl} from '../core/app_impl';
import {Trace} from '../public/trace';
import {TraceConverter} from '../public/trace_converter';

type Args =
  | UpdateStatusArgs
  | JobCompletedArgs
  | DownloadFileArgs
  | OpenTraceInLegacyArgs
  | ErrorArgs;

interface UpdateStatusArgs {
  kind: 'updateStatus';
  status: string;
}

interface JobCompletedArgs {
  kind: 'jobCompleted';
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

export class TraceConverterImpl implements TraceConverter {
  constructor(
    private readonly app: AppImpl,
    private readonly trace: Trace,
  ) {}

  async convertTraceToJsonAndDownload(): Promise<void> {
    const rawTrace = await this.trace.getTraceFile();
    return await makeWorkerAndPost(this.app, {
      kind: 'ConvertTraceAndDownload',
      trace: rawTrace,
      format: 'json',
    });
  }

  async convertTraceToSystraceAndDownload(): Promise<void> {
    const rawTrace = await this.trace.getTraceFile();
    return makeWorkerAndPost(this.app, {
      kind: 'ConvertTraceAndDownload',
      trace: rawTrace,
      format: 'systrace',
    });
  }

  async convertTraceToPprofAndDownload(pid: number, ts: time): Promise<void> {
    const rawTrace = await this.trace.getTraceFile();
    return makeWorkerAndPost(this.app, {
      kind: 'ConvertTraceToPprof',
      trace: rawTrace,
      pid,
      ts,
    });
  }
}

export function convertToJson(
  app: AppImpl,
  trace: Blob,
  openTraceInLegacy: OpenTraceInLegacyCallback,
  truncate?: 'start' | 'end',
): Promise<void> {
  return makeWorkerAndPost(
    app,
    {
      kind: 'ConvertTraceAndOpenInLegacy',
      trace,
      truncate,
    },
    openTraceInLegacy,
  );
}

async function makeWorkerAndPost(
  app: AppImpl,
  msg: unknown,
  openTraceInLegacy?: OpenTraceInLegacyCallback,
) {
  const promise = defer<void>();

  const handleOnMessage = (msg: MessageEvent) => {
    const args: Args = msg.data;
    if (args.kind === 'updateStatus') {
      app.omnibox.showStatusMessage(args.status);
    } else if (args.kind === 'jobCompleted') {
      promise.resolve();
    } else if (args.kind === 'downloadFile') {
      download({
        content: args.buffer,
        fileName: args.name,
      });
    } else if (args.kind === 'openTraceInLegacy') {
      const str = utf8Decode(args.buffer);
      openTraceInLegacy?.('trace.json', str, 0);
    } else if (args.kind === 'error') {
      app.maybeShowErrorDialog(args.error);
    } else {
      throw new Error(`Unhandled message ${JSON.stringify(args)}`);
    }
  };

  const worker = new Worker(assetSrc('traceconv_bundle.js'));
  worker.onmessage = handleOnMessage;
  worker.postMessage(msg);
  return promise;
}
