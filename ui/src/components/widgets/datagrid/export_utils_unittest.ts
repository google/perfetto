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

import {
  defaultValueFormatter,
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
} from './export_utils';

describe('export_utils', () => {
  describe('defaultValueFormatter', () => {
    it('formats null values', () => {
      expect(defaultValueFormatter(null)).toEqual('null');
    });

    it('formats string values', () => {
      expect(defaultValueFormatter('hello')).toEqual('hello');
    });

    it('formats number values', () => {
      expect(defaultValueFormatter(123)).toEqual('123');
      expect(defaultValueFormatter(123.456)).toEqual('123.456');
    });

    it('formats bigint values', () => {
      expect(defaultValueFormatter(123n)).toEqual('123');
    });

    it('formats Uint8Array values', () => {
      const blob = new Uint8Array([1, 2, 3, 4, 5]);
      expect(defaultValueFormatter(blob)).toEqual('Blob: 5 bytes');
    });
  });

  describe('formatAsTSV', () => {
    it('formats simple data as TSV', () => {
      const columns = ['colA', 'colB', 'colC'];
      const columnNames = {colA: 'colA', colB: 'colB', colC: 'colC'};
      const rows = [
        {colA: '1', colB: 'foo', colC: 'null'},
        {colA: '2', colB: 'bar', colC: '123.4'},
        {colA: '3', colB: 'baz', colC: '1'},
      ];

      const expected =
        'colA\tcolB\tcolC\n1\tfoo\tnull\n2\tbar\t123.4\n3\tbaz\t1';
      expect(formatAsTSV(columns, columnNames, rows)).toEqual(expected);
    });

    it('uses column names for headers', () => {
      const columns = ['id', 'name'];
      const columnNames = {id: 'ID', name: 'Name'};
      const rows = [{id: '1', name: 'Alice'}];

      const expected = 'ID\tName\n1\tAlice';
      expect(formatAsTSV(columns, columnNames, rows)).toEqual(expected);
    });

    it('handles empty rows', () => {
      const columns = ['a', 'b'];
      const columnNames = {a: 'a', b: 'b'};
      const rows: Array<Record<string, string>> = [];

      const expected = 'a\tb';
      expect(formatAsTSV(columns, columnNames, rows)).toEqual(expected);
    });

    it('handles empty columns', () => {
      const columns: string[] = [];
      const columnNames = {};
      const rows: Array<Record<string, string>> = [];

      const expected = '';
      expect(formatAsTSV(columns, columnNames, rows)).toEqual(expected);
    });

    it('escapes tabs and newlines in cell values', () => {
      const columns = ['col'];
      const columnNames = {col: 'col'};
      const rows = [
        {col: 'has\ttab'},
        {col: 'has\nnewline'},
        {col: 'has\r\nwindows newline'},
        {col: 'normal'},
      ];

      const result = formatAsTSV(columns, columnNames, rows);
      // Tabs and newlines should be replaced with spaces
      expect(result).not.toContain('\t\t'); // No double tabs (one is separator)
      expect(result).toContain('has tab');
      expect(result).toContain('has newline');
      expect(result).toContain('has windows newline');
      expect(result).toContain('normal');
    });
  });

  describe('formatAsJSON', () => {
    it('formats data as JSON', () => {
      const rows = [
        {colA: '1', colB: 'foo', colC: 'null'},
        {colA: '2', colB: 'bar', colC: '123.4'},
      ];

      const result = formatAsJSON(rows);
      const parsed = JSON.parse(result);
      expect(parsed).toEqual(rows);
    });

    it('formats empty array', () => {
      const rows: Array<Record<string, string>> = [];
      const expected = '[]';
      expect(formatAsJSON(rows)).toEqual(expected);
    });

    it('formats with indentation', () => {
      const rows = [{a: '1'}];
      const result = formatAsJSON(rows);
      expect(result).toContain('\n'); // Should be pretty-printed
    });
  });

  describe('formatAsMarkdown', () => {
    it('formats data as markdown table', () => {
      const columns = ['colA', 'colB', 'colC'];
      const columnNames = {colA: 'colA', colB: 'colB', colC: 'colC'};
      const rows = [
        {colA: '1', colB: 'foo', colC: 'null'},
        {colA: '2', colB: 'bar', colC: '123.4'},
        {colA: '3', colB: 'baz', colC: '1'},
      ];

      const result = formatAsMarkdown(columns, columnNames, rows);
      expect(result).toContain('| colA | colB | colC |');
      expect(result).toContain('| --- | --- | --- |');
      expect(result).toContain('| 1 | foo | null |');
      expect(result).toContain('| 2 | bar | 123.4 |');
      expect(result).toContain('| 3 | baz | 1 |');
    });

    it('escapes pipe characters', () => {
      const columns = ['col'];
      const columnNames = {col: 'col'};
      const rows = [{col: 'a|b'}];

      const result = formatAsMarkdown(columns, columnNames, rows);
      expect(result).toContain('a\\|b');
    });

    it('escapes backslashes', () => {
      const columns = ['col'];
      const columnNames = {col: 'col'};
      const rows = [{col: 'a\\b'}];

      const result = formatAsMarkdown(columns, columnNames, rows);
      expect(result).toContain('a\\\\b');
    });

    it('replaces newlines with spaces', () => {
      const columns = ['col'];
      const columnNames = {col: 'col'};
      const rows = [{col: 'has\nnewline'}, {col: 'has\r\nwindows'}];

      const result = formatAsMarkdown(columns, columnNames, rows);
      expect(result).not.toContain('\n\n'); // No double newlines
      expect(result).toContain('has newline');
      expect(result).toContain('has windows');
    });

    it('handles empty rows', () => {
      const columns = ['a', 'b'];
      const columnNames = {a: 'a', b: 'b'};
      const rows: Array<Record<string, string>> = [];

      const result = formatAsMarkdown(columns, columnNames, rows);
      expect(result).toContain('| a | b |');
      expect(result).toContain('| --- | --- |');
    });

    it('handles empty columns', () => {
      const columns: string[] = [];
      const columnNames = {};
      const rows: Array<Record<string, string>> = [];

      const expected = '';
      expect(formatAsMarkdown(columns, columnNames, rows)).toEqual(expected);
    });
  });
});
