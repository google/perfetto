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

import {defer} from '../base/deferred';
import {
  addErrorHandler,
  assertExists,
  ErrorDetails,
  reportError,
} from '../base/logging';
import {time} from '../base/time';
import {
  ConversionJobName,
  ConversionJobStatus,
} from '../common/conversion_jobs';
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

function updateJobStatus(name: ConversionJobName, status: ConversionJobStatus) {
  selfWorker.postMessage({
    kind: 'updateJobStatus',
    name,
    status,
  });
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

function fsNodeToBuffer(fsNode: traceconv.FileSystemNode): Uint8Array {
  const fileSize = assertExists(fsNode.usedBytes);
  return new Uint8Array(fsNode.contents.buffer, 0, fileSize);
}

async function runTraceconv(trace: Blob, args: string[]) {
  const deferredRuntimeInitialized = defer<void>();
  const module = traceconv({
    noInitialRun: true,
    locateFile: (s: string) => s,
    print: updateStatus,
    printErr: updateStatus,
    onRuntimeInitialized: () => deferredRuntimeInitialized.resolve(),
  });
  await deferredRuntimeInitialized;
  module.FS.mkdir('/fs');
  module.FS.mount(
    assertExists(module.FS.filesystems.WORKERFS),
    {blobs: [{name: 'trace.proto', data: trace}]},
    '/fs',
  );
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
  const jobName = format === 'json' ? 'convert_json' : 'convert_systrace';
  updateJobStatus(jobName, ConversionJobStatus.InProgress);
  const outPath = '/trace.json';
  const args: string[] = [format];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  try {
    const module = await runTraceconv(trace, args);
    const fsNode = module.FS.lookupPath(outPath).node;
    downloadFile(fsNodeToBuffer(fsNode), `trace.${format}`);
    module.FS.unlink(outPath);
  } finally {
    updateJobStatus(jobName, ConversionJobStatus.NotRunning);
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
  const jobName = 'open_in_legacy';
  updateJobStatus(jobName, ConversionJobStatus.InProgress);
  const outPath = '/trace.json';
  const args: string[] = ['json'];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  try {
    const module = await runTraceconv(trace, args);
    const fsNode = module.FS.lookupPath(outPath).node;
    const data = fsNode.contents.buffer;
    const size = fsNode.usedBytes;
    const buffer = new Uint8Array(data, 0, size);
    openTraceInLegacy(buffer);
    module.FS.unlink(outPath);
  } finally {
    updateJobStatus(jobName, ConversionJobStatus.NotRunning);
  }
}

interface ConvertTraceToPprofArgs {
  kind: 'ConvertTraceToPprof';
  trace: Blob;
  pid: number;
  ts: time;
}

function isConvertTraceToPprof(msg: Args): msg is ConvertTraceToPprofArgs {
  if (msg.kind !== 'ConvertTraceToPprof') {
    return false;
  }
  return true;
}

async function ConvertTraceToPprof(trace: Blob, pid: number, ts: time) {
  const jobName = 'convert_pprof';
  updateJobStatus(jobName, ConversionJobStatus.InProgress);
  const args = [
    'profile',
    `--pid`,
    `${pid}`,
    `--timestamps`,
    `${ts}`,
    '/fs/trace.proto',
  ];

  try {
    const module = await runTraceconv(trace, args);
    const heapDirName = Object.keys(
      module.FS.lookupPath('/tmp/').node.contents,
    )[0];
    const heapDirContents = module.FS.lookupPath(`/tmp/${heapDirName}`).node
      .contents;
    const heapDumpFiles = Object.keys(heapDirContents);
    for (let i = 0; i < heapDumpFiles.length; ++i) {
      const heapDump = heapDumpFiles[i];
      const fileNode = module.FS.lookupPath(
        `/tmp/${heapDirName}/${heapDump}`,
      ).node;
      const fileName = `/heap_dump.${i}.${pid}.pb`;
      downloadFile(fsNodeToBuffer(fileNode), fileName);
    }
  } finally {
    updateJobStatus(jobName, ConversionJobStatus.NotRunning);
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
    ConvertTraceToPprof(args.trace, args.pid, args.ts);
  } else {
    throw new Error(`Unknown method call ${JSON.stringify(args)}`);
  }
};
