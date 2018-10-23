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

import {Actions} from '../common/actions';
import * as trace_to_text from '../gen/trace_to_text';

import {globals} from './globals';

export function ConvertTrace(trace: Blob) {
  const mod = trace_to_text({
    noInitialRun: true,
    locateFile: (s: string) => s,
    print: updateStatus,
    printErr: updateStatus,
    onRuntimeInitialized: () => {
      updateStatus('Converting trace');
      const outPath = '/trace.json';
      mod.callMain(['json', '/fs/trace.proto', outPath]);
      updateStatus('Trace conversion completed');
      const fsNode = mod.FS.lookupPath(outPath).node;
      const data = fsNode.contents.buffer;
      const size = fsNode.usedBytes;
      globals.publish('LegacyTrace', {data, size}, /*transfer=*/[data]);
      mod.FS.unlink(outPath);
    },
    onAbort: () => {
      console.log('ABORT');
    },
  });
  mod.FS.mkdir('/fs');
  mod.FS.mount(
      mod.FS.filesystems.WORKERFS,
      {blobs: [{name: 'trace.proto', data: trace}]},
      '/fs');

  // TODO removeme.
  (self as {} as {mod: {}}).mod = mod;
}

function updateStatus(msg: {}) {
  console.log(msg);
  globals.dispatch(Actions.updateStatus({
    msg: msg.toString(),
    timestamp: Date.now() / 1000,
  }));
}
