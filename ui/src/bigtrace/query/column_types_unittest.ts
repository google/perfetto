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

import {describe, expect, test} from 'vitest';
import {
  decodeBase64,
  parseCellValue,
  toDataGridColumnType,
} from './column_types';

describe('column_types', () => {
  describe('toDataGridColumnType', () => {
    test('maps numeric types to quantitative', () => {
      expect(toDataGridColumnType('INT64')).toBe('quantitative');
      expect(toDataGridColumnType('UINT64')).toBe('quantitative');
      expect(toDataGridColumnType('FLOAT64')).toBe('quantitative');
    });

    test('maps string/text/date types to text', () => {
      expect(toDataGridColumnType('STRING')).toBe('text');
      expect(toDataGridColumnType('BOOL')).toBe('text');
      expect(toDataGridColumnType('DATE')).toBe('text');
      expect(toDataGridColumnType('TIMESTAMP')).toBe('text');
    });

    test('maps BYTES and unknown types to undefined', () => {
      expect(toDataGridColumnType('BYTES')).toBeUndefined();
      expect(toDataGridColumnType('UNKNOWN')).toBeUndefined();
      expect(toDataGridColumnType('')).toBeUndefined();
    });
  });

  describe('decodeBase64', () => {
    test('decodes valid base64 strings into Uint8Array', () => {
      // "hello" -> "aGVsbG8="
      const decoded = decodeBase64('aGVsbG8=');
      expect(decoded).toBeInstanceOf(Uint8Array);
      expect(Array.from(decoded!)).toEqual([104, 101, 108, 108, 111]);

      // raw bytes [255, 0, 128] -> "/wCA"
      const binary = decodeBase64('/wCA');
      expect(binary).toBeInstanceOf(Uint8Array);
      expect(Array.from(binary!)).toEqual([0xff, 0x00, 0x80]);
    });

    test('returns undefined for invalid base64', () => {
      expect(decodeBase64('!!!not-valid-b64!!!')).toBeUndefined();
    });
  });

  describe('parseCellValue', () => {
    test('handles null and undefined', () => {
      expect(parseCellValue(null, 'INT64')).toBeNull();
      expect(parseCellValue(undefined, 'INT64')).toBeNull();
      expect(parseCellValue(null, 'STRING')).toBeNull();
    });

    test('parses INT64 and UINT64 preserving precision past 2^53', () => {
      expect(parseCellValue('9223372036854775807', 'INT64')).toBe(
        9223372036854775807n,
      );
      expect(parseCellValue('-9223372036854775808', 'INT64')).toBe(
        -9223372036854775808n,
      );
      expect(parseCellValue('18446744073709551615', 'UINT64')).toBe(
        18446744073709551615n,
      );
      expect(parseCellValue('0', 'INT64')).toBe(0n);
      expect(parseCellValue('42', 'INT64')).toBe(42n);

      // Malformed int does not throw; returns null
      expect(parseCellValue('abc', 'INT64')).toBeNull();
      expect(parseCellValue('', 'INT64')).toBeNull();

      // Literal "NULL" string becomes null
      expect(parseCellValue('NULL', 'INT64')).toBeNull();
      expect(parseCellValue('NULL', 'UINT64')).toBeNull();
    });

    test('parses FLOAT64 into numbers', () => {
      expect(parseCellValue('3.14159', 'FLOAT64')).toBe(3.14159);
      expect(parseCellValue('-0.5', 'FLOAT64')).toBe(-0.5);
      expect(parseCellValue('100', 'FLOAT64')).toBe(100);
      expect(parseCellValue('Infinity', 'FLOAT64')).toBe(Infinity);
      expect(parseCellValue('-Infinity', 'FLOAT64')).toBe(-Infinity);
      expect(parseCellValue('NaN', 'FLOAT64')).toBeNaN();

      // Malformed float does not throw; returns null
      expect(parseCellValue('not-a-number', 'FLOAT64')).toBeNull();
      expect(parseCellValue('NULL', 'FLOAT64')).toBeNull();
    });

    test('parses BYTES into Uint8Array via base64 decoding', () => {
      const decoded = parseCellValue('aGVsbG8=', 'BYTES');
      expect(decoded).toBeInstanceOf(Uint8Array);
      expect(Array.from(decoded as Uint8Array)).toEqual([
        104, 101, 108, 108, 111,
      ]);

      // Literal "NULL" becomes null
      expect(parseCellValue('NULL', 'BYTES')).toBeNull();

      // Malformed base64 falls back to raw string
      expect(parseCellValue('!!!invalid!!!', 'BYTES')).toBe('!!!invalid!!!');
    });

    test('parses BOOL, DATE, TIMESTAMP as strings, respecting "NULL"', () => {
      expect(parseCellValue('true', 'BOOL')).toBe('true');
      expect(parseCellValue('false', 'BOOL')).toBe('false');
      expect(parseCellValue('NULL', 'BOOL')).toBeNull();

      expect(parseCellValue('2026-09-21', 'DATE')).toBe('2026-09-21');
      expect(parseCellValue('NULL', 'DATE')).toBeNull();

      expect(parseCellValue('2026-09-21T11:00:00Z', 'TIMESTAMP')).toBe(
        '2026-09-21T11:00:00Z',
      );
      expect(parseCellValue('NULL', 'TIMESTAMP')).toBeNull();
    });

    test('parses STRING values as-is, mapping "NULL" to null', () => {
      expect(parseCellValue('hello world', 'STRING')).toBe('hello world');
      expect(parseCellValue('123', 'STRING')).toBe('123');
      expect(parseCellValue('NULL', 'STRING')).toBeNull();
    });

    test('unknown type falls back to raw string, mapping "NULL" to null', () => {
      expect(parseCellValue('something', 'CUSTOM_TYPE')).toBe('something');
      expect(parseCellValue('NULL', 'CUSTOM_TYPE')).toBeNull();
    });
  });
});
