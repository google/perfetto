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

const columns = [
  'name',
  'displayValue',
  'displayName',
  'cpuValue',
  'gpuValue',
  'gpuName',
  'machineValue',
  'machineName',
  'processValue',
  'processName',
  'threadValue',
  'threadName',
];
type Row = Readonly<Record<string, string | null>>;

function resultFor(rows: ReadonlyArray<Row>, context: Row = {}) {
  const T = protos.QueryResult.CellsBatch.CellType;
  const values = rows.flatMap((row) =>
    columns.map((column) => ({...context, ...row})[column] ?? null),
  );
  const result = createQueryResult({query: 'dimensions'});
  result.appendResultBatch(
    protos.QueryResult.encode({
      columnNames: columns,
      batch: [
        {
          cells: values.map((value) =>
            value === null ? T.CELL_NULL : T.CELL_STRING,
          ),
          stringCells: values
            .filter((value): value is string => value !== null)
            .join('\0'),
          isLastBatch: true,
        },
      ],
    }).finish(),
  );
  return result;
}

function sectionFor(query: ReturnType<typeof vi.fn>) {
  const trace = {engine: {query}} as unknown as Trace;
  return new TrackDimensionsSection(trace);
}

function values(section: TrackDimensionsSection) {
  const root = document.createElement('div');
  m.render(root, section.render());
  const values = Array.from(
    root.querySelectorAll('.pf-tree-right'),
    (node) => node.textContent,
  );
  m.render(root, []);
  return values;
}

function selection(eventId: number): TrackEventSelection {
  return {eventId} as TrackEventSelection;
}

describe('TrackDimensionsSection', () => {
  test('queries the selected event through canonical and source args', async () => {
    const query = vi
      .fn()
      .mockResolvedValue(resultFor([{name: 'rank', displayValue: '3'}]));
    const section = sectionFor(query);
    await section.load(selection(42));
    expect(values(section)).toEqual(['3']);
    expect(query).toHaveBeenCalledOnce();
    expect(query).toHaveBeenCalledWith(
      expect.stringContaining(
        "WHERE s.id = 42 AND a.key GLOB 'custom_dimension.*'",
      ),
    );
    expect(query).toHaveBeenCalledWith(
      expect.stringContaining('label.arg_set_id = t.source_arg_set_id'),
    );
  });

  test('no custom dimensions means no section or extra probe', async () => {
    const query = vi.fn().mockResolvedValue(resultFor([], {machineValue: '0'}));
    const section = sectionFor(query);
    await section.load(selection(1));
    expect(section.render()).toBeNull();
    expect(query).toHaveBeenCalledOnce();
  });

  test('a query failure cannot break the panel or poison later selections', async () => {
    const query = vi
      .fn()
      .mockRejectedValueOnce(new Error('query failed'))
      .mockResolvedValueOnce(resultFor([{name: 'rank', displayValue: '3'}]));
    const section = sectionFor(query);
    await expect(section.load(selection(1))).resolves.toBeUndefined();
    expect(section.render()).toBeNull();
    await expect(section.load(selection(2))).resolves.toBeUndefined();
    expect(values(section)).toEqual(['3']);
    expect(query).toHaveBeenCalledTimes(2);
  });

  test('a decode failure is contained and later loads still work', async () => {
    const query = vi
      .fn()
      .mockResolvedValueOnce({
        numRows: () => 1,
        firstRow: () => {
          throw new Error('invalid result');
        },
      })
      .mockResolvedValueOnce(
        resultFor([{name: 'stage', displayValue: 'forward'}]),
      );
    const section = sectionFor(query);
    await expect(section.load(selection(1))).resolves.toBeUndefined();
    expect(section.render()).toBeNull();
    await expect(section.load(selection(2))).resolves.toBeUndefined();
    expect(values(section)).toEqual(['forward']);
  });

  test('renders entity numbers and lossless custom display text', async () => {
    const query = vi.fn().mockResolvedValue(
      resultFor(
        [
          {name: 'empty', displayValue: ''},
          {
            name: 'max',
            displayValue: '9223372036854775807',
            displayName: 'largest',
          },
          {name: 'stage', displayValue: 'forward', displayName: 'forward pass'},
        ],
        {
          cpuValue: '0',
          gpuValue: '2',
          gpuName: 'gpu',
          machineValue: '1',
          machineName: 'host',
          processValue: '100',
          processName: 'proc',
          threadValue: '101',
          threadName: 'worker',
        },
      ),
    );
    const section = sectionFor(query);
    await section.load(selection(1));
    expect(values(section)).toEqual([
      '0',
      '2 (gpu)',
      '1 (host)',
      '100 (proc)',
      '101 (worker)',
      '',
      '9223372036854775807 (largest)',
      'forward (forward pass)',
    ]);
  });

  test('missing or empty labels do not expose entity IDs', async () => {
    const query = vi.fn().mockResolvedValue(
      resultFor([{name: 'rank', displayValue: '7', displayName: ''}], {
        machineValue: '0',
        processValue: '100',
      }),
    );
    const section = sectionFor(query);
    await section.load(selection(1));
    expect(values(section)).toEqual(['0', '100', '7']);
  });
});
