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

import {TraceReadableStream} from './trace_stream';

describe('TraceReadableStream', () => {
  test('only pulls when readChunk is called', async () => {
    let pulls = 0;
    const readable = new ReadableStream<Uint8Array>(
      {
        pull(controller) {
          pulls++;
          controller.enqueue(new Uint8Array([pulls]));
          if (pulls === 2) controller.close();
        },
      },
      {highWaterMark: 0},
    );
    const stream = new TraceReadableStream(readable, 2);

    expect(pulls).toBe(0);
    await expect(stream.readChunk()).resolves.toMatchObject({
      data: new Uint8Array([1]),
      eof: false,
      bytesRead: 1,
      bytesTotal: 2,
    });
    expect(pulls).toBe(1);
    await expect(stream.readChunk()).resolves.toMatchObject({
      data: new Uint8Array([2]),
      eof: false,
      bytesRead: 2,
      bytesTotal: 2,
    });
    expect(pulls).toBe(2);
    await expect(stream.readChunk()).resolves.toMatchObject({eof: true});
  });

  test('view chunks span only the viewed bytes', async () => {
    const backing = new Uint8Array([0, 1, 2, 3, 4, 5]);
    const readable = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(backing.subarray(2, 5));
        controller.close();
      },
    });
    const stream = new TraceReadableStream(readable);

    const chunk = await stream.readChunk();
    expect(chunk.data).toEqual(new Uint8Array([2, 3, 4]));
    expect(chunk.bytesRead).toBe(3);
    expect(chunk.bytesTotal).toBe(0);
  });

  test('non-binary chunks are rejected', async () => {
    const readable = new ReadableStream<unknown>({
      start(controller) {
        controller.enqueue('not bytes');
        controller.close();
      },
    });
    const stream = new TraceReadableStream(readable);

    await expect(stream.readChunk()).rejects.toThrow(/ArrayBuffer/);
  });
});
