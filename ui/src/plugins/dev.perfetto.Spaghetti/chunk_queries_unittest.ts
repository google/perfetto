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

import {ensureDefined} from '../../base/assert';
import type {RootNodeData} from './graph_model';
import {flattenNodes} from './graph_utils';
import {serializeChunks} from './chunk_queries';
import {planChunks} from './plan_chunks';

// From(slice) -> Filter -> GroupBy -> Sort -> Limit
function chain(minDur: string): RootNodeData {
  return {
    type: 'from',
    id: 'src',
    x: 0,
    y: 0,
    config: {table: 'slice'},
    next: {
      type: 'filter',
      id: 'flt',
      config: {conditions: [{column: 'dur', op: '>', value: minDur}]},
      next: {
        type: 'groupby',
        id: 'gb',
        config: {
          groupColumns: ['name'],
          aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
        },
        next: {
          type: 'sort',
          id: 'srt',
          config: {conditions: [{column: 'cnt', order: 'DESC'}]},
          next: {type: 'limit', id: 'lim', config: {limitCount: '10'}},
        },
      },
    },
  };
}

function chunksOf(minDur: string) {
  const index = flattenNodes([chain(minDur)]);
  const res = serializeChunks(index, ensureDefined(planChunks(index, 'lim')));
  if (!res.ok) throw new Error(res.error);
  return res.chunks;
}

describe('serializeChunks', () => {
  it('serializes each chunk reading its inputs by hash', () => {
    const chunks = chunksOf('1000');
    const gb = ensureDefined(chunks[0]);
    const srt = ensureDefined(chunks[1]);
    const lim = ensureDefined(chunks[2]);
    expect(gb.sql).toBe(
      [
        'SELECT',
        '  name,',
        '  COUNT(*) AS cnt',
        'FROM slice',
        'WHERE dur > 1000',
        'GROUP BY name',
      ].join('\n'),
    );
    expect(srt.inputs).toEqual([gb.hash]);
    expect(srt.sql).toBe(`SELECT *\nFROM ${gb.hash}\nORDER BY cnt DESC`);
    expect(lim.inputs).toEqual([srt.hash]);
    expect(lim.sql).toBe(`SELECT *\nFROM ${srt.hash}\nLIMIT 10`);
  });

  it('changes every hash downstream of an edit (Merkle)', () => {
    const before = chunksOf('1000').map((c) => c.hash);
    const after = chunksOf('5000').map((c) => c.hash);
    expect(after[0]).not.toBe(before[0]);
    expect(after[1]).not.toBe(before[1]);
    expect(after[2]).not.toBe(before[2]);
    expect(chunksOf('1000').map((c) => c.hash)).toEqual(before);
  });
});
