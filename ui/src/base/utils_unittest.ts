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

import {toArrayBuffer} from './utils';

describe('toArrayBuffer', () => {
  test('passes through a pure ArrayBuffer unchanged', () => {
    const buf = new Uint8Array([1, 2, 3, 4]).buffer;
    expect(toArrayBuffer(buf)).toBe(buf);
  });

  test('unwraps a full-span Uint8Array without copying', () => {
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const out = toArrayBuffer(u8);
    expect(out).toBeInstanceOf(ArrayBuffer);
    expect(out).toBe(u8.buffer);
    expect(new Uint8Array(out)).toEqual(u8);
  });

  test('slices out a view with a non-zero byteOffset', () => {
    const backing = new Uint8Array([0, 1, 2, 3, 4, 5]).buffer;
    // View covering bytes [2, 4).
    const view = new Uint8Array(backing, 2, 2);
    const out = toArrayBuffer(view);
    expect(out).toBeInstanceOf(ArrayBuffer);
    expect(out.byteLength).toBe(2);
    expect(new Uint8Array(out)).toEqual(new Uint8Array([2, 3]));
    // Must be a fresh buffer, not the oversized backing one.
    expect(out).not.toBe(backing);
  });

  test('slices out a view shorter than its backing buffer', () => {
    const backing = new Uint8Array([9, 8, 7, 6]).buffer;
    const view = new Uint8Array(backing, 0, 2);
    const out = toArrayBuffer(view);
    expect(out.byteLength).toBe(2);
    expect(new Uint8Array(out)).toEqual(new Uint8Array([9, 8]));
  });

  test('handles a DataView', () => {
    const backing = new Uint8Array([1, 2, 3, 4]).buffer;
    const dv = new DataView(backing, 1, 2);
    const out = toArrayBuffer(dv);
    expect(new Uint8Array(out)).toEqual(new Uint8Array([2, 3]));
  });
});
