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
import m from 'mithril';
import protos from '../../protos';
import {createQueryResult} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {TrackEventSelection} from '../../public/selection';
import {TrackDimensionsSection} from './track_dimensions_section';

describe('TrackDimensionsSection', () => {
  test('a failing query does not break later loads', async () => {
    // E.g. a trace processor without `track_dimension`.
    let queries = 0;
    const trace = {
      engine: {
        query: () => {
          ++queries;
          return Promise.reject(new Error('no such table: track_dimension'));
        },
      },
    } as unknown as Trace;
    const selection = {eventId: 1} as TrackEventSelection;

    const section = new TrackDimensionsSection(trace);
    await expect(section.load(selection)).resolves.toBeUndefined();
    expect(section.render()).toBeNull();

    // The failure is treated as "no custom dimensions" for the whole trace.
    await expect(section.load(selection)).resolves.toBeUndefined();
    expect(section.render()).toBeNull();
    expect(queries).toBe(1);
  });

  test('an older dimension schema is checked only once', async () => {
    const query = vi.fn((sql: string) => {
      if (sql.includes('display_value')) {
        return Promise.reject(new Error('no such column: display_value'));
      }
      return Promise.resolve({firstRow: () => ({has_custom: 1})});
    });
    const trace = {engine: {query}} as unknown as Trace;
    const selection = {eventId: 1} as TrackEventSelection;

    for (let i = 0; i < 2; ++i) {
      const section = new TrackDimensionsSection(trace);
      await expect(section.load(selection)).resolves.toBeUndefined();
      expect(section.render()).toBeNull();
    }
    expect(query).toHaveBeenCalledOnce();
  });

  test('a failing probe decoder does not break later loads', async () => {
    const firstRow = vi.fn(() => {
      throw new Error('missing has_custom column');
    });
    const query = vi.fn().mockResolvedValue({firstRow});
    const trace = {engine: {query}} as unknown as Trace;
    const selection = {eventId: 1} as TrackEventSelection;

    for (let i = 0; i < 2; ++i) {
      const section = new TrackDimensionsSection(trace);
      await expect(section.load(selection)).resolves.toBeUndefined();
      expect(section.render()).toBeNull();
    }
    expect(query).toHaveBeenCalledOnce();
    expect(firstRow).toHaveBeenCalledOnce();
  });

  test('a failing event query does not disable later selections', async () => {
    const T = protos.QueryResult.CellsBatch.CellType;
    const result = createQueryResult({query: 'dimensions'});
    result.appendResultBatch(
      protos.QueryResult.encode({
        columnNames: ['name', 'display_value', 'display_name', 'is_well_known'],
        batch: [
          {
            cells: [T.CELL_STRING, T.CELL_STRING, T.CELL_NULL, T.CELL_VARINT],
            stringCells: ['rank', '3'].join('\0'),
            varintCells: [0],
            isLastBatch: true,
          },
        ],
      }).finish(),
    );
    const query = vi
      .fn()
      .mockResolvedValueOnce({firstRow: () => ({has_custom: 1})})
      .mockRejectedValueOnce(new Error('query failed'))
      .mockResolvedValueOnce(result);
    const trace = {engine: {query}} as unknown as Trace;
    const section = new TrackDimensionsSection(trace);

    await expect(
      section.load({eventId: 1} as TrackEventSelection),
    ).resolves.toBeUndefined();
    expect(section.render()).toBeNull();

    const next = new TrackDimensionsSection(trace);
    await expect(
      next.load({eventId: 2} as TrackEventSelection),
    ).resolves.toBeUndefined();
    expect(next.render()).not.toBeNull();
    expect(query).toHaveBeenCalledTimes(3);
  });

  test('renders stdlib display values without losing large integers', async () => {
    const T = protos.QueryResult.CellsBatch.CellType;
    const result = createQueryResult({query: 'dimensions'});
    result.appendResultBatch(
      protos.QueryResult.encode({
        columnNames: ['name', 'display_value', 'display_name', 'is_well_known'],
        batch: [
          {
            cells: [
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_NULL,
              T.CELL_VARINT,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_VARINT,
              T.CELL_STRING,
              T.CELL_STRING,
              T.CELL_NULL,
              T.CELL_VARINT,
            ],
            stringCells: [
              'process',
              '100',
              'proc',
              'cpu',
              '0',
              'max',
              '9223372036854775807',
              'largest',
              'stage',
              'forward',
              'forward pass',
              'empty',
              '',
            ].join('\0'),
            varintCells: [1, 1, 0, 0, 0],
            isLastBatch: true,
          },
        ],
      }).finish(),
    );
    const query = vi
      .fn()
      .mockResolvedValueOnce({firstRow: () => ({has_custom: 1})})
      .mockResolvedValueOnce(result);
    const trace = {engine: {query}} as unknown as Trace;
    const section = new TrackDimensionsSection(trace);
    await section.load({eventId: 1} as TrackEventSelection);

    const root = document.createElement('div');
    m.render(root, section.render());
    expect(
      Array.from(
        root.querySelectorAll('.pf-tree-right'),
        (node) => node.textContent,
      ),
    ).toEqual([
      '100 (proc)',
      '0',
      '9223372036854775807 (largest)',
      'forward (forward pass)',
      '',
    ]);
    m.render(root, []);
  });
});
