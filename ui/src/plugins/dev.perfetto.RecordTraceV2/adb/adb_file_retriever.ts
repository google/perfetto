// Copyright (C) 2025 The Android Open Source Project
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

import {defer} from '../../../base/deferred';
import {FileRetriever} from '../tracing_protocol/long_trace_tracing_session';
import {AdbDevice} from './adb_device';

/**
 * Implements FileRetriever for ADB targets. Uses `exec:cat` to stream file
 * contents as raw binary (unlike `shell:`, the `exec:` service does not
 * allocate a PTY, so binary data is not corrupted). File size is obtained
 * beforehand via `stat` to report progress as a percentage. Chunks are
 * written directly to the provided FileSystemWritableFileStream, keeping
 * memory usage constant regardless of trace size.
 */
export class AdbFileRetriever implements FileRetriever {
  constructor(private readonly adbDevice: AdbDevice) {}

  async pullFile(
    path: string,
    writable: FileSystemWritableFileStream,
    onProgress: (pct: number) => void,
  ): Promise<{ok: true} | {ok: false; error: string}> {
    // Get the file size so we can report progress as a percentage.
    const sizeResult = await this.adbDevice.shell(`stat -c %s ${path}`);
    if (!sizeResult.ok) {
      return {ok: false, error: `Failed to stat file: ${sizeResult.error}`};
    }
    const totalSize = parseInt(sizeResult.value.trim(), 10);
    if (isNaN(totalSize) || totalSize <= 0) {
      return {ok: false, error: `Invalid file size: ${sizeResult.value}`};
    }

    // Use exec: (not shell:) to avoid PTY binary corruption.
    const streamResult = await this.adbDevice.createStream(`exec:cat ${path}`);
    if (!streamResult.ok) {
      return {ok: false, error: streamResult.error};
    }

    const stream = streamResult.value;
    let bytesReceived = 0;
    let writeError: string | undefined;
    const done = defer<void>();

    stream.onData = (data: Uint8Array) => {
      bytesReceived += data.length;
      // Write chunks to disk. writable.write() queues internally so this is
      // safe to call without awaiting each write.
      writable.write(data).catch((e) => {
        writeError = `${e}`;
      });
      onProgress(Math.min(Math.round((100 * bytesReceived) / totalSize), 100));
    };
    stream.onClose = () => done.resolve();
    await done;

    if (writeError !== undefined) {
      return {ok: false, error: `Write failed: ${writeError}`};
    }
    if (bytesReceived === 0) {
      return {ok: false, error: 'Empty response from exec:cat'};
    }
    return {ok: true};
  }

  async deleteFile(path: string): Promise<void> {
    await this.adbDevice.shell(`rm -f ${path}`);
  }
}
