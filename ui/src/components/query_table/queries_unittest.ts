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
  formatAsDelimited,
  formatAsMarkdownTable,
  ResponseLike,
} from './queries';

describe('query conversion', () => {
  const FAKE_RESPONSE: ResponseLike = {
    columns: ['colA', 'colB', 'colC'],
    rows: [
      {colA: 1, colB: 'foo', colC: null},
      {colA: 2, colB: 'bar', colC: 123.4},
      {colA: 3, colB: 'baz', colC: 1},
    ],
  };

  describe('formatAsDelimited', () => {
    it('converts to delimited', () => {
      const expected =
        'colA\tcolB\tcolC\n1\tfoo\tNULL\n2\tbar\t123.4\n3\tbaz\t1';
      expect(formatAsDelimited(FAKE_RESPONSE)).toEqual(expected);
    });

    it('converts to delimited with custom separator', () => {
      const expected = 'colA,colB,colC\n1,foo,NULL\n2,bar,123.4\n3,baz,1';
      expect(formatAsDelimited(FAKE_RESPONSE, ',')).toEqual(expected);
    });

    it('handles empty delimited conversion', () => {
      const resp: ResponseLike = {columns: [], rows: []};
      expect(formatAsDelimited(resp)).toEqual('');
    });

    it('handles delimited conversion with no rows', () => {
      const resp: ResponseLike = {columns: ['a', 'b'], rows: []};
      expect(formatAsDelimited(resp)).toEqual('a\tb');
    });
  });

  describe('formatAsMarkdownTable', () => {
    it('converts to markdown', () => {
      const expected = `| colA | colB | colC  |
| ---- | ---- | ----- |
| 1    | foo  | NULL  |
| 2    | bar  | 123.4 |
| 3    | baz  | 1     |`;
      expect(formatAsMarkdownTable(FAKE_RESPONSE)).toEqual(expected);
    });

    it('handles empty markdown conversion', () => {
      const resp: ResponseLike = {columns: [], rows: []};
      expect(formatAsMarkdownTable(resp)).toEqual('');
    });

    it('handles markdown conversion with no rows', () => {
      const resp: ResponseLike = {columns: ['a', 'b'], rows: []};
      const expected = `| a   | b   |
| --- | --- |`;
      expect(formatAsMarkdownTable(resp)).toEqual(expected);
    });
  });
});
