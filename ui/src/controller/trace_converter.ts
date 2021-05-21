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

import {defer} from '../base/deferred';
import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {ConversionJobStatus} from '../common/conversion_jobs';
import {TraceSource} from '../common/state';
import * as trace_to_text from '../gen/trace_to_text';

import {globals} from './globals';

type Format = 'json'|'systrace';

export function ConvertTrace(
    trace: Blob, format: Format, truncate?: 'start'|'end') {
  const jobName = 'open_in_legacy';
  globals.publish('ConversionJobStatusUpdate', {
    jobName,
    jobStatus: ConversionJobStatus.InProgress,
  });
  const outPath = '/trace.json';
  const args: string[] = [format];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  runTraceconv(trace, args)
      .then(module => {
        const fsNode = module.FS.lookupPath(outPath).node;
        const data = fsNode.contents.buffer;
        const size = fsNode.usedBytes;
        globals.publish('LegacyTrace', {data, size}, /*transfer=*/[data]);
        module.FS.unlink(outPath);
      })
      .finally(() => {
        globals.publish('ConversionJobStatusUpdate', {
          jobName,
          jobStatus: ConversionJobStatus.NotRunning,
        });
      });
}

export function ConvertTraceAndDownload(
    trace: Blob, format: Format, truncate?: 'start'|'end') {
  const jobName = `convert_${format}`;
  globals.publish('ConversionJobStatusUpdate', {
    jobName,
    jobStatus: ConversionJobStatus.InProgress,
  });
  const outPath = '/trace.json';
  const args: string[] = [format];
  if (truncate !== undefined) {
    args.push('--truncate', truncate);
  }
  args.push('/fs/trace.proto', outPath);
  runTraceconv(trace, args)
      .then(module => {
        const fsNode = module.FS.lookupPath(outPath).node;
        downloadFile(fsNodeToBlob(fsNode), `trace.${format}`);
        module.FS.unlink(outPath);
      })
      .finally(() => {
        globals.publish('ConversionJobStatusUpdate', {
          jobName,
          jobStatus: ConversionJobStatus.NotRunning,
        });
      });
}

export function ConvertTraceToPprof(
    pid: number, src: TraceSource, ts1: number, ts2?: number) {
  const jobName = 'convert_pprof';
  globals.publish('ConversionJobStatusUpdate', {
    jobName,
    jobStatus: ConversionJobStatus.InProgress,
  });
  const timestamps = `${ts1}${ts2 === undefined ? '' : `,${ts2}`}`;
  const args = [
    'profile',
    `--pid`,
    `${pid}`,
    `--timestamps`,
    timestamps,
    '/fs/trace.proto'
  ];
  generateBlob(src)
      .then(traceBlob => {
        runTraceconv(traceBlob, args).then(module => {
          const heapDirName =
              Object.keys(module.FS.lookupPath('/tmp/').node.contents)[0];
          const heapDirContents =
              module.FS.lookupPath(`/tmp/${heapDirName}`).node.contents;
          const heapDumpFiles = Object.keys(heapDirContents);
          let fileNum = 0;
          heapDumpFiles.forEach(heapDump => {
            const fileNode =
                module.FS.lookupPath(`/tmp/${heapDirName}/${heapDump}`).node;
            fileNum++;
            const fileName = `/heap_dump.${fileNum}.${pid}.pb`;
            downloadFile(fsNodeToBlob(fileNode), fileName);
          });
        });
      })
      .finally(() => {
        globals.publish('ConversionJobStatusUpdate', {
          jobName,
          jobStatus: ConversionJobStatus.NotRunning,
        });
      });
}

async function runTraceconv(trace: Blob, args: string[]) {
  const deferredRuntimeInitialized = defer<void>();
  const module = trace_to_text({
    noInitialRun: true,
    locateFile: (s: string) => s,
    print: updateStatus,
    printErr: updateStatus,
    onRuntimeInitialized: () => deferredRuntimeInitialized.resolve()
  });
  await deferredRuntimeInitialized;
  module.FS.mkdir('/fs');
  module.FS.mount(
      assertExists(module.FS.filesystems.WORKERFS),
      {blobs: [{name: 'trace.proto', data: trace}]},
      '/fs');
  updateStatus('Converting trace');
  module.callMain(args);
  updateStatus('Trace conversion completed');
  return module;
}

async function generateBlob(src: TraceSource) {
  let blob: Blob = new Blob();
  if (src.type === 'URL') {
    const resp = await fetch(src.url);
    if (resp.status !== 200) {
      throw new Error(`fetch() failed with HTTP error ${resp.status}`);
    }
    blob = await resp.blob();
  } else if (src.type === 'ARRAY_BUFFER') {
    blob = new Blob([new Uint8Array(src.buffer, 0, src.buffer.byteLength)]);
  } else if (src.type === 'FILE') {
    blob = src.file;
  } else {
    throw new Error(`Conversion not supported for ${JSON.stringify(src)}`);
  }
  return blob;
}

function fsNodeToBlob(fsNode: trace_to_text.FileSystemNode): Blob {
  const fileSize = assertExists(fsNode.usedBytes);
  const bufView = new Uint8Array(fsNode.contents.buffer, 0, fileSize);
  return new Blob([bufView]);
}

function downloadFile(file: Blob, name: string) {
  globals.publish('FileDownload', {file, name});
}

function updateStatus(msg: {}) {
  console.log(msg);
  globals.dispatch(Actions.updateStatus({
    msg: msg.toString(),
    timestamp: Date.now() / 1000,
  }));
}
