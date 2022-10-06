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

import {
  decode as b64Decode,
  encode as b64Encode,
  length as b64Len,
} from '@protobufjs/base64';
import {
  length as utf8Len,
  read as utf8Read,
  write as utf8Write,
} from '@protobufjs/utf8';

import {assertTrue} from './logging';

// TextDecoder/Decoder requires the full DOM and isn't available in all types
// of tests. Use fallback implementation from protbufjs.
let Utf8Decoder: {decode: (buf: Uint8Array) => string;};
let Utf8Encoder: {encode: (str: string) => Uint8Array;};
try {
  Utf8Decoder = new TextDecoder('utf-8');
  Utf8Encoder = new TextEncoder();
} catch (_) {
  if (typeof process === 'undefined') {
    // Silence the warning when we know we are running under NodeJS.
    console.warn(
        'Using fallback UTF8 Encoder/Decoder, This should happen only in ' +
        'tests and NodeJS-based environments, not in browsers.');
  }
  Utf8Decoder = {decode: (buf: Uint8Array) => utf8Read(buf, 0, buf.length)};
  Utf8Encoder = {
    encode: (str: string) => {
      const arr = new Uint8Array(utf8Len(str));
      const written = utf8Write(str, arr, 0);
      assertTrue(written === arr.length);
      return arr;
    },
  };
}

export function base64Encode(buffer: Uint8Array): string {
  return b64Encode(buffer, 0, buffer.length);
}

export function base64Decode(str: string): Uint8Array {
  // if the string is in base64url format, convert to base64
  const b64 = str.replace(/-/g, '+').replace(/_/g, '/');
  const arr = new Uint8Array(b64Len(b64));
  const written = b64Decode(b64, arr, 0);
  assertTrue(written === arr.length);
  return arr;
}

// encode binary array to hex string
export function hexEncode(bytes: Uint8Array): string {
  return bytes.reduce(
      (prev, cur) => prev + ('0' + cur.toString(16)).slice(-2), '');
}

export function utf8Encode(str: string): Uint8Array {
  return Utf8Encoder.encode(str);
}

// Note: not all byte sequences can be converted to<>from UTF8. This can be
// used only with valid unicode strings, not arbitrary byte buffers.
export function utf8Decode(buffer: Uint8Array): string {
  return Utf8Decoder.decode(buffer);
}

// The binaryEncode/Decode functions below allow to encode an arbitrary binary
// buffer into a string that can be JSON-encoded. binaryEncode() applies
// UTF-16 encoding to each byte individually.
// Unlike utf8Encode/Decode, any arbitrary byte sequence can be converted into a
// valid string, and viceversa.
// This should be only used when a byte array needs to be transmitted over an
// interface that supports only JSON serialization (e.g., postmessage to a
// chrome extension).

export function binaryEncode(buf: Uint8Array): string {
  let str = '';
  for (let i = 0; i < buf.length; i++) {
    str += String.fromCharCode(buf[i]);
  }
  return str;
}

export function binaryDecode(str: string): Uint8Array {
  const buf = new Uint8Array(str.length);
  const strLen = str.length;
  for (let i = 0; i < strLen; i++) {
    buf[i] = str.charCodeAt(i);
  }
  return buf;
}

// A function used to interpolate strings into SQL query. The only replacement
// is done is that single quote replaced with two single quotes, according to
// SQLite documentation:
// https://www.sqlite.org/lang_expr.html#literal_values_constants_
//
// The purpose of this function is to use in simple comparisons, to escape
// strings used in GLOB clauses see escapeQuery function.
export function sqliteString(str: string): string {
  return `'${str.replace(/'/g, '\'\'')}'`;
}
