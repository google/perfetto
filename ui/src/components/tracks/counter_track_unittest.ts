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

import {vi} from 'vitest';
import protos from '../../protos';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {okResult} from '../../base/result';
import {Time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import type {Trace} from '../../public/trace';
import type {TrackRenderContext} from '../../public/track';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import {CounterTrack} from './counter_track';

const CellType = protos.QueryResult.CellsBatch.CellType;

// Builds a QueryResult with the given numeric rows.
function numericResult(columnNames: string[], rows: number[][]): QueryResult {
  const batch = protos.QueryResult.CellsBatch.create({
    cells: rows.flatMap((row) => row.map(() => CellType.CELL_FLOAT64)),
    float64Cells: rows.flat(),
    isLastBatch: true,
  });
  const result = createQueryResult({query: ''});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({columnNames, batch: [batch]}),
    ).finish(),
  );
  return result;
}

// Records all queries and answers the ones issued by CounterTrack.
class FakeEngine {
  readonly log: string[] = [];

  // Rows returned by the global limits query; empty if the source is empty.
  limitsRows = [[0, 10]];

  async query(sql: string): Promise<QueryResult> {
    this.log.push(sql);
    if (sql.includes('AS minDisplayValue')) {
      return numericResult(
        ['minDisplayValue', 'maxDisplayValue', 'tsRel', 'lastDisplayValue'],
        [
          [1, 2, 0, 2],
          [3, 4, 500, 4],
        ],
      );
    }
    if (sql.includes('min_value AS min')) {
      return numericResult(['min', 'max'], this.limitsRows);
    }
    return numericResult([], []);
  }

  async tryQuery(sql: string) {
    return okResult(await this.query(sql));
  }

  createdTables(): string[] {
    return this.log.filter((q) => q.startsWith('CREATE VIRTUAL TABLE'));
  }

  droppedTables(): string[] {
    return this.log.filter((q) => q.startsWith('DROP TABLE'));
  }
}

function tableName(createStatement: string): string {
  return createStatement.split(' ')[3];
}

function setup() {
  const engine = new FakeEngine();
  const trace = {
    engine,
    taskTracker: {track: <T>(p: Promise<T>) => p},
  } as unknown as Trace;
  const drawStepArea = vi.fn();
  const visibleWindow = new HighPrecisionTimeSpan(
    new HighPrecisionTime(Time.ZERO),
    1000,
  );

  // A canvas context whose methods are no-ops.
  const ctx = new Proxy(
    {},
    {
      get: (_, prop) =>
        prop === 'measureText'
          ? () => ({
              width: 0,
              actualBoundingBoxAscent: 0,
              actualBoundingBoxDescent: 0,
              actualBoundingBoxLeft: 0,
              actualBoundingBoxRight: 0,
            })
          : () => {},
      set: () => true,
    },
  );

  const renderCtx = {
    visibleWindow,
    size: {width: 100, height: 40},
    resolution: 16n,
    ctx,
    timescale: new TimeScale(visibleWindow, {left: 0, right: 100}),
    colors: {},
    renderer: {drawStepArea},
  } as unknown as TrackRenderContext;

  return {engine, trace, drawStepArea, renderCtx};
}

// Renders the track a few times, letting its async queries run in between.
async function renderUntilSettled(
  track: CounterTrack,
  renderCtx: TrackRenderContext,
) {
  for (let i = 0; i < 5; i++) {
    track.render(renderCtx);
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
}

test('static sqlSource builds the mipmap table once', async () => {
  const {engine, trace, drawStepArea, renderCtx} = setup();
  const track = CounterTrack.create({
    trace,
    uri: 'test',
    sqlSource: 'SELECT ts, value FROM a',
  });

  await renderUntilSettled(track, renderCtx);
  await renderUntilSettled(track, renderCtx);

  expect(engine.createdTables()).toHaveLength(1);
  expect(engine.droppedTables()).toHaveLength(0);
  expect(drawStepArea).toHaveBeenCalled();
});

test('dynamic sqlSource rebuilds the tables when it changes', async () => {
  const {engine, trace, drawStepArea, renderCtx} = setup();
  let sqlSource = 'SELECT ts, value FROM a';
  const track = CounterTrack.create({
    trace,
    uri: 'test',
    sqlSource: () => sqlSource,
  });

  await renderUntilSettled(track, renderCtx);
  const [first] = engine.createdTables();
  expect(first).toContain('FROM a');

  // While the table is rebuilt, the previous data is still drawn.
  sqlSource = 'SELECT ts, value FROM b';
  drawStepArea.mockClear();
  track.render(renderCtx);
  expect(drawStepArea).toHaveBeenCalledTimes(1);

  // The old table is dropped and data is fetched from the new one.
  await renderUntilSettled(track, renderCtx);
  const created = engine.createdTables();
  expect(created).toHaveLength(2);
  expect(created[1]).toContain('FROM b');
  expect(engine.droppedTables()).toEqual([
    `DROP TABLE IF EXISTS ${tableName(first)}`,
  ]);
  const dataQueries = engine.log.filter((q) => q.includes('minDisplayValue'));
  expect(dataQueries.at(-1)).toContain(tableName(created[1]));
});

test('dynamic sqlSource changing on every frame is drawn', async () => {
  const {engine, trace, drawStepArea, renderCtx} = setup();
  let generation = 0;
  const track = CounterTrack.create({
    trace,
    uri: 'test',
    sqlSource: () => `SELECT ts, value FROM a WHERE gen <= ${generation++}`,
  });

  // Each source is drawn once loaded, then the latest one is loaded.
  await renderUntilSettled(track, renderCtx);
  await renderUntilSettled(track, renderCtx);
  expect(drawStepArea).toHaveBeenCalled();
  expect(engine.createdTables().length).toBeGreaterThan(1);
});

test('materialization evaluates a dynamic sqlSource once', async () => {
  const {engine, trace} = setup();
  const sqlSource = vi.fn(() => 'SELECT ts, value FROM a');

  await CounterTrack.createMaterialized({trace, uri: 'test', sqlSource});

  expect(sqlSource).toHaveBeenCalledTimes(1);
  expect(engine.log).toHaveLength(1);
  expect(engine.log[0]).toContain('AS SELECT ts, value FROM a');
});

test('empty sources render without errors', async () => {
  const {engine, trace, renderCtx} = setup();
  engine.limitsRows = [];
  const track = CounterTrack.create({
    trace,
    uri: 'test',
    sqlSource: 'SELECT ts, value FROM a',
  });

  await renderUntilSettled(track, renderCtx);

  expect(() => track.render(renderCtx)).not.toThrow();
});
