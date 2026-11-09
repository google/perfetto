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

import type {RootNodeData} from './graph_model';
import {flattenNodes} from './graph_utils';
import {planChunks} from './plan_chunks';

// From(slice) -> Filter -> GroupBy -> Filter -> Sort -> Limit
const chain: RootNodeData = {
  type: 'from',
  id: 'src',
  x: 0,
  y: 0,
  config: {table: 'slice'},
  next: {
    type: 'filter',
    id: 'flt1',
    config: {conditions: [{column: 'dur', op: '>', value: '1000'}]},
    next: {
      type: 'groupby',
      id: 'gb',
      config: {
        groupColumns: ['name'],
        aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
      },
      next: {
        type: 'filter',
        id: 'flt2',
        config: {conditions: [{column: 'cnt', op: '>', value: '5'}]},
        next: {
          type: 'sort',
          id: 'srt',
          config: {conditions: [{column: 'cnt', order: 'DESC'}]},
          next: {type: 'limit', id: 'lim', config: {limitCount: '10'}},
        },
      },
    },
  },
};

describe('planChunks', () => {
  it('cuts a chain at every expensive node', () => {
    const plan = planChunks(flattenNodes([chain]), 'lim');
    expect(plan).toEqual({
      chunks: [
        {rootId: 'gb', nodeIds: ['src', 'flt1', 'gb'], inputs: []},
        {rootId: 'srt', nodeIds: ['flt2', 'srt'], inputs: ['gb']},
        {rootId: 'lim', nodeIds: ['lim'], inputs: ['srt']},
      ],
      outputId: 'lim',
    });
  });

  it('roots the output chunk at an expensive target', () => {
    const plan = planChunks(flattenNodes([chain]), 'srt');
    expect(plan?.outputId).toBe('srt');
    expect(plan?.chunks.map((c) => c.rootId)).toEqual(['gb', 'srt']);
  });

  it('repeats shared cheap nodes and builds shared expensive nodes once', () => {
    // src -> flt (cheap, read by both srt and lim_b) -> srt (expensive);
    // a union reads srt (through lim_a) and flt (through lim_b).
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'filter',
          id: 'flt',
          config: {conditions: [{column: 'dur', op: '>', value: '0'}]},
          next: {
            type: 'sort',
            id: 'srt',
            config: {conditions: [{column: 'dur', order: 'DESC'}]},
          },
        },
      },
      {
        type: 'limit',
        id: 'lim_a',
        x: 0,
        y: 0,
        inputs: ['srt'],
        config: {limitCount: '1'},
      },
      {
        type: 'limit',
        id: 'lim_b',
        x: 0,
        y: 0,
        inputs: ['flt'],
        config: {limitCount: '2'},
      },
      {
        type: 'union',
        id: 'uni',
        x: 0,
        y: 0,
        inputs: ['lim_a', 'lim_b'],
        config: {numInputs: 2, distinct: false},
      },
    ];
    const plan = planChunks(flattenNodes(nodes), 'uni');
    expect(plan?.chunks.map((c) => [c.rootId, c.nodeIds, c.inputs])).toEqual([
      ['srt', ['src', 'flt', 'srt'], []],
      ['uni', ['lim_a', 'src', 'flt', 'lim_b', 'uni'], ['srt']],
    ]);
  });

  it('keeps a node read by several ports of one node in its chunk', () => {
    const nodes: RootNodeData[] = [
      {
        type: 'from',
        id: 'src',
        x: 0,
        y: 0,
        config: {table: 'slice'},
        next: {
          type: 'filter',
          id: 'flt',
          config: {conditions: [{column: 'dur', op: '>', value: '0'}]},
        },
      },
      {
        type: 'union',
        id: 'uni',
        x: 0,
        y: 0,
        inputs: ['flt', 'flt'],
        config: {numInputs: 2, distinct: false},
      },
    ];
    const plan = planChunks(flattenNodes(nodes), 'uni');
    expect(plan?.chunks.map((c) => [c.rootId, c.nodeIds, c.inputs])).toEqual([
      ['uni', ['src', 'flt', 'uni'], []],
    ]);
  });

  it('chunks incomplete nodes like any other', () => {
    const plan = planChunks(
      flattenNodes([
        {
          type: 'from',
          id: 'src',
          x: 0,
          y: 0,
          config: {table: ''},
          next: {type: 'limit', id: 'lim', config: {limitCount: 'abc'}},
        },
      ]),
      'lim',
    );
    expect(plan?.chunks.map((c) => c.nodeIds)).toEqual([['src', 'lim']]);
  });

  it('returns undefined for unknown node types', () => {
    const index = flattenNodes([
      {type: 'nope', id: 'x', x: 0, y: 0, config: {}} as RootNodeData,
    ]);
    expect(planChunks(index, 'x')).toBeUndefined();
    expect(planChunks(index, 'missing')).toBeUndefined();
  });
});
