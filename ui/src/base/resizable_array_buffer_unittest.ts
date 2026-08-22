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

import {pseudoRand} from './rand';
import {ResizableArrayBuffer} from './resizable_array_buffer';

describe('ResizableArrayBuffer', () => {
  test('simple', () => {
    const buf = new ResizableArrayBuffer();
    buf.append([1, 2]);
    expect(buf.get()).toEqual(new Uint8Array([1, 2]));

    buf.append([3]);
    expect(buf.get()).toEqual(new Uint8Array([1, 2, 3]));

    buf.clear();
    expect(buf.get().length).toEqual(0);
    expect(buf.get()).toEqual(new Uint8Array([]));
  });

  test('get', () => {
    const buf = new ResizableArrayBuffer();
    buf.append([1, 2, 3]);

    // get() returns a correctly-sized view of the used portion.
    const view = buf.get();
    expect(buf.capacity).toBeGreaterThan(3); // Sanity check
    expect(view).toEqual(new Uint8Array([1, 2, 3]));
    expect(view.length).toEqual(3);

    // It is a live view into the backing buffer: writing through it mutates the
    // underlying storage.
    view[0] = 9;
    expect(buf.get()).toEqual(new Uint8Array([9, 2, 3]));
  });

  test('getBuffer', () => {
    const buf = new ResizableArrayBuffer();
    buf.append([1, 2, 3]);

    // The size (3) is smaller than the capacity (128), so getBuffer() must
    // return a copy sized exactly to the used portion, not the whole backing
    // buffer.
    const ab = buf.getBuffer();
    expect(buf.capacity).toBeGreaterThan(3); // Sanity check
    expect(ab.byteLength).toEqual(3);
    expect(new Uint8Array(ab)).toEqual(new Uint8Array([1, 2, 3]));

    // The returned buffer must be a copy: mutating the source must not affect
    // it.
    buf.append([4]);
    expect(new Uint8Array(ab)).toEqual(new Uint8Array([1, 2, 3]));
  });

  test('merge', () => {
    const buf = new ResizableArrayBuffer();

    // Append to the buffer in batches of variables sizes, filling with a
    // pseudo-random sequence.
    const randState = {seed: 1};
    const randU8 = () => (pseudoRand(randState) * 256) & 0xff;
    const BATCH_SIZES = [11, 129, 1023, 1024 * 7];
    const TOTAL_SIZE = BATCH_SIZES.reduce((a, s) => a + s, 0);
    const expected = new Uint8Array(TOTAL_SIZE);
    let e = 0;
    for (const batchSize of BATCH_SIZES) {
      const batch = new Uint8Array(batchSize);
      for (let i = 0; i < batch.length; i++) {
        const num = randU8();
        batch[i] = num;
        expected[e++] = num;
      }
      buf.append(batch);
    }

    // Now reset the seed and check that the random sequence of the merged array
    // matches.
    const merged = buf.get();
    expect(merged.length).toEqual(expected.length);
    expect(merged).toEqual(expected);
  });
});
