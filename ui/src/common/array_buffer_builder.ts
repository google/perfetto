// Copyright (C) 2022 The Android Open Source Project
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

import {
  length as utf8Len,
  write as utf8Write,
} from '@protobufjs/utf8';

import {assertTrue} from '../base/logging';
import {isString} from '../base/object_utils';

// A token that can be appended to an `ArrayBufferBuilder`.
export type ArrayBufferToken = string|number|Uint8Array;

// Return the length, in bytes, of a token to be inserted.
function tokenLength(token: ArrayBufferToken): number {
  if (isString(token)) {
    return utf8Len(token);
  } else if (token instanceof Uint8Array) {
    return token.byteLength;
  } else {
    assertTrue(token >= 0 && token <= 0xffffffff);
    // 32-bit integers take 4 bytes
    return 4;
  }
}

// Insert a token into the buffer, at position `byteOffset`.
//
// @param dataView A DataView into the buffer to write into.
// @param typedArray A Uint8Array view into the buffer to write into.
// @param byteOffset Position to write at, in the buffer.
// @param token Token to insert into the buffer.
function insertToken(
    dataView: DataView,
    typedArray: Uint8Array,
    byteOffset: number,
    token: ArrayBufferToken): void {
  if (isString(token)) {
    // Encode the string in UTF-8
    const written = utf8Write(token, typedArray, byteOffset);
    assertTrue(written === utf8Len(token));
  } else if (token instanceof Uint8Array) {
    // Copy the bytes from the other array
    typedArray.set(token, byteOffset);
  } else {
    assertTrue(token >= 0 && token <= 0xffffffff);
    // 32-bit little-endian value
    dataView.setUint32(byteOffset, token, true);
  }
}

// Like a string builder, but for an ArrayBuffer instead of a string. This
// allows us to assemble messages to send/receive over the wire. Data can be
// appended to the buffer using `append()`. The data we append can be of the
// following types:
//
// - string: the ASCII string is appended. Throws an error if there are
//           non-ASCII characters.
// - number: the number is appended as a 32-bit little-endian integer.
// - Uint8Array: the bytes are appended as-is to the buffer.
export class ArrayBufferBuilder {
  private readonly tokens: ArrayBufferToken[] = [];

  // Return an `ArrayBuffer` that is the concatenation of all the tokens.
  toArrayBuffer(): ArrayBuffer {
    // Calculate the size of the buffer we need.
    let byteLength = 0;
    for (const token of this.tokens) {
      byteLength += tokenLength(token);
    }
    // Allocate the buffer.
    const buffer = new ArrayBuffer(byteLength);
    const dataView = new DataView(buffer);
    const typedArray = new Uint8Array(buffer);
    // Fill the buffer with the tokens.
    let byteOffset = 0;
    for (const token of this.tokens) {
      insertToken(dataView, typedArray, byteOffset, token);
      byteOffset += tokenLength(token);
    }
    assertTrue(byteOffset === byteLength);
    // Return the values.
    return buffer;
  }

  // Add one or more tokens to the value of this object.
  append(token: ArrayBufferToken): void {
    this.tokens.push(token);
  }
}
