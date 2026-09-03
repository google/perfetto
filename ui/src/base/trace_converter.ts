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

import type {ErrorDetails} from './logging';
import type {time} from './time';
import {
  runTraceconv,
  type PprofProfileType,
  type WorkerRequest,
} from './traceconv_bundle_proxy';

export type ConversionResult =
  | {
      readonly ok: true;
      readonly result: {
        readonly buffer: Uint8Array<ArrayBuffer>;
        readonly name: string;
      };
    }
  | {readonly ok: false; readonly error: ErrorDetails};

export type {PprofProfileType};

interface ConvertTraceOptsBase {
  readonly onStatus?: (status: string) => void;
}

export type ConvertTraceOpts = ConvertTraceOptsBase &
  (
    | {
        readonly format: 'json' | 'systrace';
        readonly truncate?: 'start' | 'end';
      }
    | {
        readonly format: 'pprof';
        readonly profileType: PprofProfileType;
        readonly pid: number;
        readonly ts: time;
      }
  );

/**
 * Runs traceconv in a worker to convert a trace. The |opts.format| tag
 * determines the conversion:
 * - 'json' / 'systrace': exports the trace in the given format.
 * - 'pprof': extracts a pprof profile for the given pid/timestamp.
 *
 * Resolves with the converted file contents and a suggested filename;
 * conversion failures are reported via the {ok: false} variant.
 */
export async function convertTrace(
  trace: Blob,
  opts: ConvertTraceOpts,
): Promise<ConversionResult> {
  const request: WorkerRequest =
    opts.format === 'pprof'
      ? {
          kind: 'ConvertTraceToPprof',
          trace,
          profileType: opts.profileType,
          pid: opts.pid,
          ts: opts.ts,
        }
      : {
          kind: 'ConvertTraceAndDownload',
          trace,
          format: opts.format,
          truncate: opts.truncate,
        };

  return await new Promise((res) => {
    runTraceconv(request, {
      onStatus: opts.onStatus,
      onError: (error: ErrorDetails) => {
        res({ok: false, error});
      },
      onDownloadFile: (buffer, name) => {
        res({ok: true, result: {buffer, name}});
      },
    });
  });
}
