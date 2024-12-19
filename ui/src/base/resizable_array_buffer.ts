// Copyright (C) 2024 The Android Open Source Project
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

import {assertTrue} from './logging';

/**
 * A dynamically resizable array buffer implementation for efficient
 * storage and manipulation of binary data. It starts with a specified
 * initial size and grows as needed to accommodate appended data.
 * Efficiently grows the buffer using an exponential strategy up to 32MB,
 * and then linearly in 32MB increments to minimize reallocation overhead.
 * Provides methods to append data, shrink the size, clear the buffer,
 * and retrieve the stored data as a `Uint8Array`.
 */
export class ResizableArrayBuffer {
  private buf: Uint8Array;
  private _size = 0;

  constructor(private readonly initialSize = 128) {
    this.buf = new Uint8Array(initialSize);
  }

  append(data: ArrayLike<number>) {
    const capacityNeeded = this._size + data.length;
    if (this.capacity < capacityNeeded) {
      this.grow(capacityNeeded);
    }
    this.buf.set(data, this._size);
    this._size = capacityNeeded;
  }

  shrink(newSize: number) {
    assertTrue(newSize <= this._size);
    this._size = newSize;
  }

  clear() {
    this.buf = new Uint8Array(this.initialSize);
    this._size = 0;
  }

  get(): Uint8Array {
    return this.buf.subarray(0, this._size);
  }

  get size(): number {
    return this._size;
  }

  get capacity(): number {
    return this.buf.length;
  }

  private grow(capacityNeeded: number) {
    let newSize = this.buf.length;
    const MB32 = 32 * 1024 * 1024;
    do {
      newSize = newSize < MB32 ? newSize * 2 : newSize + MB32;
    } while (newSize < capacityNeeded);
    const newBuf = new Uint8Array(newSize);
    assertTrue(newBuf.length >= capacityNeeded);
    newBuf.set(this.buf);
    this.buf = newBuf;
  }
}
