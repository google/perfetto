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

import {describe, expect, test, vi} from 'vitest';
import protos from '../../protos';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {createQueryResult} from '../../trace_processor/query_result';
import type ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {addDimensionLabels} from './dimensions';

const T = protos.QueryResult.CellsBatch.CellType;

describe('addDimensionLabels', () => {
  test('a missing dimensions table does not reject the load', async () => {
    const query = vi
      .fn()
      .mockRejectedValue(new Error('no such table: track_dimension'));
    const trace = {engine: {query}} as unknown as Trace;
    const groups = {} as ProcessThreadGroupsPlugin;

    await expect(
      addDimensionLabels(trace, new Map(), groups),
    ).resolves.toBeUndefined();
    expect(query).toHaveBeenCalledOnce();
  });

  test('a result decoding failure leaves existing subtitles intact', async () => {
    const result = createQueryResult({query: 'dimensions'});
    result.appendResultBatch(
      protos.QueryResult.encode({
        columnNames: [],
        batch: [{isLastBatch: true}],
      }).finish(),
    );
    const query = vi.fn().mockResolvedValue(result);
    const trace = {engine: {query}} as unknown as Trace;
    const groups = {} as ProcessThreadGroupsPlugin;
    const node = new TrackNode({subtitle: 'existing'});

    await expect(
      addDimensionLabels(trace, new Map([[1, node]]), groups),
    ).resolves.toBeUndefined();
    expect(node.subtitle).toBe('existing');
  });

  test('labels process groups and suppresses inherited labels on multiplexed nodes', async () => {
    const result = createQueryResult({query: 'dimensions'});
    result.appendResultBatch(
      protos.QueryResult.encode({
        columnNames: [
          'trackId',
          'processUpid',
          'name',
          'intValue',
          'stringValue',
          'displayName',
        ],
        batch: [
          {
            cells: [
              T.CELL_VARINT,
              T.CELL_VARINT,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_NULL,
              T.CELL_NULL,
              T.CELL_VARINT,
              T.CELL_NULL,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_NULL,
              T.CELL_NULL,
              T.CELL_VARINT,
              T.CELL_NULL,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_NULL,
              T.CELL_NULL,
            ],
            varintCells: [11, 10, 3, 12, 3, 13, 7],
            stringCells: 'rank\0rank\0rank',
            isLastBatch: true,
          },
        ],
      }).finish(),
    );
    const trace = {
      engine: {query: vi.fn().mockResolvedValue(result)},
    } as unknown as Trace;
    const group = new TrackNode({subtitle: 'example.com'});
    const child = new TrackNode();
    group.addChildLast(child);
    const other = new TrackNode();
    const getGroupForProcess = vi.fn().mockReturnValue(group);
    const groups = {getGroupForProcess} as unknown as ProcessThreadGroupsPlugin;

    await addDimensionLabels(
      trace,
      new Map([
        [11, child],
        [12, child],
        [13, other],
      ]),
      groups,
    );

    expect(getGroupForProcess).toHaveBeenCalledWith(10);
    expect(group.subtitle).toBe('example.com · rank 3');
    expect(child.subtitle).toBeUndefined();
    expect(other.subtitle).toBe('rank 7');
  });
});
