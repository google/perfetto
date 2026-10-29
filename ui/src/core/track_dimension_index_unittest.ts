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
import protos from '../protos';
import type {Track, TrackTags} from '../public/track';
import {TrackNode} from '../public/workspace';
import {createQueryResult} from '../trace_processor/query_result';
import {TrackDimensionIndex} from './track_dimension_index';

interface Row {
  trackId: number;
  name: string;
  value: bigint | string;
  displayName?: string;
}

function resultFor(rows: ReadonlyArray<Row>) {
  const T = protos.QueryResult.CellsBatch.CellType;
  const cells: number[] = [];
  const integers: Array<number | {low: number; high: number}> = [];
  const strings: string[] = [];
  for (const row of rows) {
    cells.push(T.CELL_VARINT, T.CELL_STRING);
    integers.push(row.trackId);
    strings.push(row.name);
    if (typeof row.value === 'bigint') {
      cells.push(T.CELL_VARINT, T.CELL_NULL);
      const bits = BigInt.asUintN(64, row.value);
      integers.push({
        low: Number(bits & 0xffffffffn),
        high: Number(bits >> 32n),
      });
    } else {
      cells.push(T.CELL_NULL, T.CELL_STRING);
      strings.push(row.value);
    }
    cells.push(row.displayName === undefined ? T.CELL_NULL : T.CELL_STRING);
    if (row.displayName !== undefined) strings.push(row.displayName);
    cells.push(T.CELL_STRING);
    strings.push(String(row.value));
  }
  const result = createQueryResult({query: 'dimensions'});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({
        columnNames: [
          'trackId',
          'name',
          'intValue',
          'stringValue',
          'displayName',
          'displayValue',
        ],
        batch: [
          {
            cells,
            // Encode exact 32-bit word pairs without converting int64 values
            // through the generated number-only API.
            varintCells: integers as unknown as number[],
            stringCells: strings.join('\0'),
            isLastBatch: true,
          },
        ],
      }),
    ).finish(),
  );
  return result;
}

async function indexFor(rows: ReadonlyArray<Row>) {
  const query = vi.fn().mockResolvedValue(resultFor(rows));
  const index = new TrackDimensionIndex({query});
  await index.initialize();
  return {index, query};
}

function registered(tags: TrackTags): Track {
  return {uri: 'track', tags, renderer: {render: () => {}}};
}

function chips(index: TrackDimensionIndex, ids: ReadonlyArray<number>) {
  return index.getChips(new TrackNode({uri: 'track'}), () =>
    registered({trackIds: ids, dimensionSourceTrackIds: ids}),
  );
}

describe('TrackDimensionIndex', () => {
  test('loads once and performs render lookups without more SQL', async () => {
    const {index, query} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    await index.initialize();
    expect(chips(index, [1])).toEqual(['rank 3']);
    expect(chips(index, [2])).toEqual(['rank 7']);
    expect(query).toHaveBeenCalledOnce();
    expect(query).toHaveBeenCalledWith(
      expect.stringContaining('WHERE is_well_known = 0'),
    );
  });

  test('event IDs alone do not opt a plugin into dimension chips', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    const node = new TrackNode({uri: 'other'});
    expect(index.getChips(node, () => registered({trackIds: [1]}))).toEqual([]);
    expect(
      index.getChips(node, () => ({
        ...registered({trackIds: [1], dimensionSourceTrackIds: [1]}),
        pluginId: 'com.example.Other',
      })),
    ).toEqual(['rank 3']);
  });

  test('an ancestor without opt-in does not suppress a visible chip', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    const parent = new TrackNode({uri: 'parent'});
    const child = new TrackNode({uri: 'child'});
    parent.addChildLast(child);
    const lookup = (uri: string) =>
      registered(
        uri === 'parent'
          ? {trackIds: [1]}
          : {trackIds: [1], dimensionSourceTrackIds: [1]},
      );
    expect(index.getChips(child, lookup)).toEqual(['rank 3']);
  });

  test('hides globally uniform dimensions of either type', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 3n},
      {trackId: 1, name: 'stage', value: 'forward'},
      {trackId: 2, name: 'stage', value: 'forward'},
    ]);
    expect(chips(index, [1])).toEqual([]);
  });

  test('formats display names and sorts dimensions independently', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'stage', value: 'forward'},
      {trackId: 1, name: 'rank', value: 3n, displayName: 'worker-east'},
      {trackId: 2, name: 'stage', value: 'backward'},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    expect(chips(index, [1])).toEqual(['worker-east', 'stage forward']);
  });

  test('distinguishes integer and string canonical values', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: '3'},
    ]);
    expect(chips(index, [1])).toEqual(['rank 3']);
    expect(chips(index, [1, 2])).toEqual([]);
  });

  test('preserves adjacent int64 values beyond number precision', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 9007199254740992n},
      {trackId: 2, name: 'rank', value: 9007199254740993n},
    ]);
    expect(chips(index, [1])).toEqual(['rank 9007199254740992']);
    expect(chips(index, [2])).toEqual(['rank 9007199254740993']);
  });

  test('requires agreement across every represented track', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 3n},
      {trackId: 3, name: 'rank', value: 7n},
    ]);
    expect(chips(index, [1, 2, 1])).toEqual(['rank 3']);
    expect(chips(index, [1, 3])).toEqual([]);
    expect(chips(index, [1, 99])).toEqual([]);
    expect(chips(index, [])).toEqual([]);
  });

  test('does not select one conflicting display label', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n, displayName: 'east'},
      {trackId: 2, name: 'rank', value: 3n, displayName: 'west'},
      {trackId: 3, name: 'rank', value: 7n},
    ]);
    expect(chips(index, [1, 2])).toEqual([]);
  });

  test('explicit sources override event IDs, including an empty override', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    const node = new TrackNode({uri: 'summary'});
    expect(
      index.getChips(node, () =>
        registered({trackIds: [1], dimensionSourceTrackIds: [2]}),
      ),
    ).toEqual(['rank 7']);
    expect(
      index.getChips(node, () =>
        registered({trackIds: [1], dimensionSourceTrackIds: []}),
      ),
    ).toEqual([]);
  });

  test('suppresses ancestor dimensions, but not on a parentless pin', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 3n},
      {trackId: 3, name: 'rank', value: 7n},
    ]);
    const parent = new TrackNode({uri: 'summary'});
    const child = new TrackNode({uri: 'child'});
    parent.addChildLast(child);
    const lookup = (uri: string) =>
      registered(
        uri === 'summary'
          ? {dimensionSourceTrackIds: [1]}
          : {trackIds: [2], dimensionSourceTrackIds: [2]},
      );
    expect(index.getChips(parent, lookup)).toEqual(['rank 3']);
    expect(index.getChips(child, lookup)).toEqual([]);
    expect(index.getChips(child.clone(), lookup)).toEqual(['rank 3']);
  });

  test('hidden headless ancestors do not suppress visible labels', async () => {
    const {index} = await indexFor([
      {trackId: 1, name: 'rank', value: 3n},
      {trackId: 2, name: 'rank', value: 7n},
    ]);
    const parent = new TrackNode({uri: 'headless', headless: true});
    const child = new TrackNode({uri: 'child'});
    parent.addChildLast(child);
    const lookup = () =>
      registered({trackIds: [1], dimensionSourceTrackIds: [1]});
    expect(index.getChips(child, lookup)).toEqual(['rank 3']);
    expect(index.getChips(child, lookup, true)).toEqual([]);
  });

  test('query and decoding errors reject initialization', async () => {
    const error = new Error('no such table: track_dimension');
    const index = new TrackDimensionIndex({
      query: vi.fn().mockRejectedValue(error),
    });
    await expect(index.initialize()).rejects.toBe(error);
    const empty = createQueryResult({query: 'dimensions'});
    empty.appendResultBatch(
      protos.QueryResult.encode({batch: [{isLastBatch: true}]}).finish(),
    );
    const malformed = new TrackDimensionIndex({
      query: vi.fn().mockResolvedValue(empty),
    });
    await expect(malformed.initialize()).rejects.toThrow();
  });
});
