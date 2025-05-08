// Copyright (C) 2019 The Android Open Source Project
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

import {defer, Deferred} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';
import {exists} from '../base/utils';

const SLICE_SIZE = 32 * 1024 * 1024;

// The object returned by TraceStream.readChunk() promise.
export interface TraceChunk {
  data: Uint8Array;
  eof: boolean;
  bytesRead: number;
  bytesTotal: number;
}

// Base interface for loading trace data in chunks.
// The caller has to call readChunk() until TraceChunk.eof == true.
export interface TraceStream {
  readChunk(): Promise<TraceChunk>;
}

// Loads a trace from a File object. For the "open file" use case.
export class TraceFileStream implements TraceStream {
  private traceFile: Blob;
  private reader: FileReader;
  private pendingRead?: Deferred<TraceChunk>;
  private bytesRead = 0;

  constructor(traceFile: Blob) {
    this.traceFile = traceFile;
    this.reader = new FileReader();
    this.reader.onloadend = () => this.onLoad();
  }

  private onLoad() {
    const pendingRead = assertExists(this.pendingRead);
    this.pendingRead = undefined;
    if (this.reader.error) {
      pendingRead.reject(this.reader.error);
      return;
    }
    const res = assertExists(this.reader.result) as ArrayBuffer;
    this.bytesRead += res.byteLength;
    pendingRead.resolve({
      data: new Uint8Array(res),
      eof: this.bytesRead >= this.traceFile.size,
      bytesRead: this.bytesRead,
      bytesTotal: this.traceFile.size,
    });
  }

  readChunk(): Promise<TraceChunk> {
    const sliceEnd = Math.min(this.bytesRead + SLICE_SIZE, this.traceFile.size);
    const slice = this.traceFile.slice(this.bytesRead, sliceEnd);
    this.pendingRead = defer<TraceChunk>();
    this.reader.readAsArrayBuffer(slice);
    return this.pendingRead;
  }
}

// Loads a trace from an ArrayBuffer. For the window.open() + postMessage
// use-case, used by other dashboards (see post_message_handler.ts).
export class TraceBufferStream implements TraceStream {
  private traceBuf: ArrayBuffer;
  private bytesRead = 0;

  constructor(traceBuf: ArrayBuffer) {
    // This is subtle. Technically speaking a Uint8Array is type-compatible with
    // ArrayBuffer, so you can pass here a Uint8Array rather than an ArrayBufer.
    // However, the new Uint8Array(buf, off, len) below works only if the arg
    // is an ArrayBuffer, and silently ignores the (off,len) if the argument is
    // a Uint8Array. We could try to deal gracefully with both cases in this
    // class, but then other parts of the code (e.g. cache_manager.ts) will have
    // similar bugs if traceInfo.source is not a pure ArrayBuffer.
    // See b/390473162.
    assertTrue(traceBuf instanceof ArrayBuffer);
    this.traceBuf = traceBuf;
  }

  readChunk(): Promise<TraceChunk> {
    assertTrue(this.bytesRead <= this.traceBuf.byteLength);
    const len = Math.min(SLICE_SIZE, this.traceBuf.byteLength - this.bytesRead);
    const data = new Uint8Array(this.traceBuf, this.bytesRead, len);
    this.bytesRead += len;
    return Promise.resolve({
      data,
      eof: this.bytesRead >= this.traceBuf.byteLength,
      bytesRead: this.bytesRead,
      bytesTotal: this.traceBuf.byteLength,
    });
  }
}

// Loads a stream from a URL via fetch(). For the permalink (?s=UUID) and
// open url (?url=http://...) cases.
export class TraceHttpStream implements TraceStream {
  private bytesRead = 0;
  private bytesTotal = 0;
  private uri: string;
  private httpStream?: ReadableStreamDefaultReader<Uint8Array>;

  constructor(uri: string) {
    assertTrue(uri.startsWith('http://') || uri.startsWith('https://'));
    this.uri = uri;
  }

  async readChunk(): Promise<TraceChunk> {
    // Initialize the fetch() job on the first read request.
    if (this.httpStream === undefined) {
      const response = await fetch(this.uri);
      if (response.status !== 200) {
        throw new Error(`HTTP ${response.status} - ${response.statusText}`);
      }
      const len = response.headers.get('Content-Length');
      this.bytesTotal = exists(len) ? Number.parseInt(len, 10) : 0;
      this.httpStream = response.body!.getReader();
    }

    let eof = false;
    let bytesRead = 0;
    const chunks = [];

    // httpStream can return very small chunks which can slow down
    // TraceProcessor. Here we accumulate chunks until we get at least 32mb
    // or hit EOF.
    while (!eof && bytesRead < 32 * 1024 * 1024) {
      const res = await this.httpStream.read();
      if (res.value) {
        chunks.push(res.value);
        bytesRead += res.value.length;
      }
      eof = res.done;
    }

    let data;
    if (chunks.length === 1) {
      data = chunks[0];
    } else {
      // Stitch all the chunks into one big array:
      data = new Uint8Array(bytesRead);
      let offset = 0;
      for (const chunk of chunks) {
        data.set(chunk, offset);
        offset += chunk.length;
      }
    }

    this.bytesRead += data.length;

    return {
      data,
      eof,
      bytesRead: this.bytesRead,
      bytesTotal: this.bytesTotal,
    };
  }
}
