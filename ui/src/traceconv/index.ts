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

import {addErrorHandler, type ErrorDetails, reportError} from '../base/logging';
import type {time} from '../base/time';
import traceconv from '../gen/traceconv';

const selfWorker = self as {} as Worker;

// TODO(hjd): The trace ends up being copied too many times due to how
// blob works. We should reduce the number of copies.

type Format = 'json' | 'systrace';
type Args =
  | ConvertTraceAndDownloadArgs
  | ConvertTraceAndOpenInLegacyArgs
  | ConvertTraceToPprofArgs;

function updateStatus(status: string) {
  selfWorker.postMessage({
    kind: 'updateStatus',
    status,
  });
}

function notifyJobCompleted() {
  selfWorker.postMessage({kind: 'jobCompleted'});
}

function downloadFile(buffer: Uint8Array, name: string) {
  selfWorker.postMessage(
    {
      kind: 'downloadFile',
      buffer,
      name,
    },
    [buffer.buffer],
  );
}

function openTraceInLegacy(buffer: Uint8Array) {
  selfWorker.postMessage({
    kind: 'openTraceInLegacy',
    buffer,
  });
}

function forwardError(error: ErrorDetails) {
  selfWorker.postMessage({
    kind: 'error',
    error,
  });
}

async function runTraceconv(trace: Blob, args: string[], outDir?: string) {
  const module = await traceconv({
    noInitialRun: true,
    locateFile: (s: string) => s,
    print: updateStatus,
    printErr: updateStatus,
    onRuntimeInitialized: () => {},
  });
  // Only the explicitly exported functions survive Closure Compiler: it
  // renames the methods of module.FS and the properties of the nodes they
  // return, so neither may be touched from here.
  module.FS_mkdir('/fs');
  module.FS_mount(
    module.WORKERFS,
    {blobs: [{name: 'trace.proto', data: trace}]},
    '/fs',
  );
  if (outDir !== undefined) {
    // Created up front so it can be listed even when traceconv writes
    // nothing into it.
    module.FS_mkdir(outDir);
  }
  updateStatus('Converting trace');
  module.callMain(args);
  updateStatus('Trace conversion completed');
  return module;
}

interface ConvertTraceAndDownloadArgs {
  kind: 'ConvertTraceAndDownload';
  trace: Blob;
  format: Format;
  truncate?: 'start' | 'end';
}

function isConvertTraceAndDownload(
  msg: Args,
): msg is ConvertTraceAndDownloadArgs {
  if (msg.kind !== 'ConvertTraceAndDownload') {
    return false;
  }
  if (msg.trace === undefined) {
    throw new Error('ConvertTraceAndDownloadArgs missing trace');
  }
  if (msg.format !== 'json' && msg.format !== 'systrace') {
    throw new Error('ConvertTraceAndDownloadArgs has bad format');
  }
  return true;
}

async function ConvertTraceAndDownload(
  trace: Blob,
  format: Format,
  truncate?: 'start' | 'end',
): Promise<void> {
  const outPath = '/trace.json';
  const args: string[] = [format];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  try {
    const module = await runTraceconv(trace, args);
    downloadFile(module.FS_readFile(outPath), `trace.${format}`);
    module.FS_unlink(outPath);
  } finally {
    notifyJobCompleted();
  }
}

interface ConvertTraceAndOpenInLegacyArgs {
  kind: 'ConvertTraceAndOpenInLegacy';
  trace: Blob;
  truncate?: 'start' | 'end';
}

function isConvertTraceAndOpenInLegacy(
  msg: Args,
): msg is ConvertTraceAndOpenInLegacyArgs {
  if (msg.kind !== 'ConvertTraceAndOpenInLegacy') {
    return false;
  }
  return true;
}

async function ConvertTraceAndOpenInLegacy(
  trace: Blob,
  truncate?: 'start' | 'end',
) {
  const outPath = '/trace.json';
  const args: string[] = ['json'];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  try {
    const module = await runTraceconv(trace, args);
    openTraceInLegacy(module.FS_readFile(outPath));
    module.FS_unlink(outPath);
  } finally {
    notifyJobCompleted();
  }
}

type PprofProfileType = 'alloc' | 'perf' | 'java-heap';

interface ConvertTraceToPprofArgs {
  kind: 'ConvertTraceToPprof';
  trace: Blob;
  profileType: PprofProfileType;
  pid: number;
  ts: time;
}

function isConvertTraceToPprof(msg: Args): msg is ConvertTraceToPprofArgs {
  if (msg.kind !== 'ConvertTraceToPprof') {
    return false;
  }
  return true;
}

async function ConvertTraceToPprof(
  trace: Blob,
  profileType: PprofProfileType,
  pid: number,
  ts: time,
) {
  // Name the destination rather than letting traceconv invent a random
  // directory under /tmp: the profiles have to be found again from here.
  const outDir = '/profiles';
  const args = [
    'profile',
    `--${profileType}`,
    `--pid`,
    `${pid}`,
    `--timestamps`,
    `${ts}`,
    `--output-dir`,
    outDir,
    '/fs/trace.proto',
  ];

  try {
    const module = await runTraceconv(trace, args, outDir);
    const profiles = module
      .FS_readdir(outDir)
      .filter((name) => name !== '.' && name !== '..');
    if (profiles.length === 0) {
      throw new Error(
        'No profiles generated; the trace has no profile matching ' +
          `type=${profileType} pid=${pid} ts=${ts}`,
      );
    }
    for (const profile of profiles) {
      // traceconv already names each file after the profile it holds.
      downloadFile(module.FS_readFile(`${outDir}/${profile}`), profile);
    }
  } finally {
    notifyJobCompleted();
  }
}

selfWorker.onmessage = (msg: MessageEvent) => {
  self.addEventListener('error', (e) => reportError(e));
  self.addEventListener('unhandledrejection', (e) => reportError(e));
  addErrorHandler((error: ErrorDetails) => forwardError(error));
  const args = msg.data as Args;
  if (isConvertTraceAndDownload(args)) {
    ConvertTraceAndDownload(args.trace, args.format, args.truncate);
  } else if (isConvertTraceAndOpenInLegacy(args)) {
    ConvertTraceAndOpenInLegacy(args.trace, args.truncate);
  } else if (isConvertTraceToPprof(args)) {
    ConvertTraceToPprof(args.trace, args.profileType, args.pid, args.ts);
  } else {
    throw new Error(`Unknown method call ${JSON.stringify(args)}`);
  }
};
