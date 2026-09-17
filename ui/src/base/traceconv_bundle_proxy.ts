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

import {assetSrc} from './assets';
import {defer} from './deferred';
import type {ErrorDetails} from './logging';
import type {time} from './time';
import {utf8Decode} from './string_utils';

type Args =
  | UpdateStatusArgs
  | JobCompletedArgs
  | DownloadFileArgs
  | OpenTraceInLegacyArgs
  | ErrorArgs;

export type PprofProfileType = 'alloc' | 'perf' | 'java-heap';

export type WorkerRequest =
  | ConvertTraceAndDownloadRequest
  | ConvertTraceAndOpenInLegacyRequest
  | ConvertTraceToPprofRequest;

export interface ConvertTraceAndDownloadRequest {
  kind: 'ConvertTraceAndDownload';
  trace: Blob;
  format: 'json' | 'systrace';
  truncate?: 'start' | 'end';
}

export interface ConvertTraceAndOpenInLegacyRequest {
  kind: 'ConvertTraceAndOpenInLegacy';
  trace: Blob;
  truncate?: 'start' | 'end';
}

export interface ConvertTraceToPprofRequest {
  kind: 'ConvertTraceToPprof';
  trace: Blob;
  profileType: PprofProfileType;
  pid: number;
  ts: time;
}

interface UpdateStatusArgs {
  kind: 'updateStatus';
  status: string;
}

interface JobCompletedArgs {
  kind: 'jobCompleted';
}

interface DownloadFileArgs {
  kind: 'downloadFile';
  buffer: Uint8Array<ArrayBuffer>;
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

export interface WorkerCallbacks {
  readonly onStatus?: (status: string) => void;
  readonly onError?: (error: ErrorDetails) => void;
  readonly onDownloadFile?: (
    buffer: Uint8Array<ArrayBuffer>,
    name: string,
  ) => void;
  readonly onOpenTraceInLegacy?: OpenTraceInLegacyCallback;
}

// A thin wrapper around the traceconv bundle. Simply runs the bundle in a
// worker, sends the request, and calls the various callbacks when the worker
// sends the relevant messages back.
export async function runTraceconv(
  msg: WorkerRequest,
  callbacks?: WorkerCallbacks,
) {
  const promise = defer<void>();

  function handleOnMessage(msg: MessageEvent): void {
    const args: Args = msg.data;
    if (args.kind === 'updateStatus') {
      callbacks?.onStatus?.(args.status);
    } else if (args.kind === 'jobCompleted') {
      promise.resolve();
    } else if (args.kind === 'downloadFile') {
      callbacks?.onDownloadFile?.(args.buffer, args.name);
    } else if (args.kind === 'openTraceInLegacy') {
      const str = utf8Decode(args.buffer);
      callbacks?.onOpenTraceInLegacy?.('trace.json', str, 0);
    } else if (args.kind === 'error') {
      callbacks?.onError?.(args.error);
    } else {
      throw new Error(`Unhandled message ${JSON.stringify(args)}`);
    }
  }

  const worker = new Worker(assetSrc('traceconv_bundle.js'));
  worker.onmessage = handleOnMessage;
  worker.postMessage(msg);
  return promise;
}
