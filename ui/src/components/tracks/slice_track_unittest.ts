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

import {SourceDataset} from '../../trace_processor/dataset';
import {
  createQueryResult,
  LONG,
  LONG_NULL,
  NUM,
  type QueryResult,
} from '../../trace_processor/query_result';
import {generateRenderQuery, SliceTrack} from './slice_track';
import {vi} from 'vitest';
import protos from '../../protos';
import {okResult} from '../../base/result';
import {Time} from '../../base/time';
import {HighPrecisionTime} from '../../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {TimeScale} from '../../base/time_scale';
import type {Trace} from '../../public/trace';
import type {TrackRenderContext, TrackRenderer} from '../../public/track';
import {GRAY} from '../colorizer';

describe('generateRenderQuery', () => {
  test('minimal query', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {ts: LONG},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT ts AS ts, ROW_NUMBER() OVER (ORDER BY ts) AS id, 0 AS layer, 0 AS depth, 0 AS dur FROM (${dataset.query()})`,
    );
  });

  test('full query', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, layer: NUM},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, dur AS dur, depth AS depth, layer AS layer FROM (${dataset.query()})`,
    );
  });

  test('no dur, no depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, 0 AS depth, 0 AS dur FROM (${dataset.query()})`,
    );
  });

  test('no depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM, dur: LONG},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, dur AS dur, internal_layout(ts, dur) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth FROM (${dataset.query()})`,
    );
  });

  test('nullable dur, do depth', () => {
    const dataset = new SourceDataset({
      src: 'foo',
      schema: {id: NUM, ts: LONG, layer: NUM, dur: LONG_NULL},
    });
    expect(generateRenderQuery(dataset)).toBe(
      `SELECT id AS id, ts AS ts, layer AS layer, COALESCE(dur, -1) AS dur, internal_layout(ts, COALESCE(dur, -1)) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth FROM (${dataset.query()})`,
    );
  });
});

describe('SliceTrack compressed row interactions', () => {
  const timescale = new TimeScale(
    new HighPrecisionTimeSpan(new HighPrecisionTime(Time.ZERO), 100),
    {left: 0, right: 100},
  );

  function setup(collapsed = true, customClick = false) {
    const selectTrackEvent = vi.fn();
    const onSliceClick = vi.fn();
    const trace = {
      timeline: {},
      selection: {selectTrackEvent},
      raf: {scheduleFullRedraw: vi.fn()},
    } as unknown as Trace;
    const track = SliceTrack.create({
      trace,
      uri: 'test',
      dataset: new SourceDataset({src: 'slices', schema: {ts: LONG}}),
      initialMaxDepth: 1,
      sliceLayout: {collapsed},
      ...(customClick && {onSliceClick}),
    });
    // Seed fetched data to exercise actual hit testing without a SQL engine.
    Object.assign(track, {
      currentDataFrame: {
        start: Time.ZERO,
        end: Time.fromRaw(100n),
        slices: {
          starts: new Float32Array([0, 10]),
          ends: new Float32Array([100, 90]),
          depths: new Uint16Array([0, 1]),
          patterns: new Uint8Array(2),
          slices: [0, 1].map((id) => ({
            id,
            title: `Frame ${id}`,
            subtitle: '',
            count: 1,
            colorScheme: GRAY,
            fillRatio: 1,
            row: {ts: 0n},
          })),
          count: 2,
        },
        instants: {count: 0},
      },
    });
    track.getHeight();
    return {track, selectTrackEvent, onSliceClick};
  }

  test('first compressed-row click expands; next click selects', () => {
    const {track, selectTrackEvent} = setup();
    const event = {x: 50, y: 22, timescale};
    track.onMouseMove(event);
    expect(track.renderTooltip()).toBe('Click to expand rows');
    expect(track.onMouseClick(event)).toBe(true);
    expect(selectTrackEvent).not.toHaveBeenCalled();
    expect(track.getHeight()).toBe(42);
    expect(track.getSliceVerticalBounds(1)).toEqual({top: 21, bottom: 39});
    expect(track.onMouseClick(event)).toBe(true);
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 1);
  });

  test.each([10, 21])('top row at y=%s selects without expanding', (y) => {
    const {track, selectTrackEvent} = setup();
    track.onMouseClick({x: 50, y, timescale});
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 0);
    expect(track.getHeight()).toBe(27);
  });

  test('empty space in compressed rows does not expand', () => {
    const {track, selectTrackEvent} = setup();
    expect(track.onMouseClick({x: 5, y: 22, timescale})).toBe(false);
    expect(selectTrackEvent).not.toHaveBeenCalled();
    expect(track.getHeight()).toBe(27);
  });

  test('expansion precedes custom click callbacks', () => {
    const {track, onSliceClick} = setup(true, true);
    const event = {x: 50, y: 22, timescale};
    track.onMouseClick(event);
    expect(onSliceClick).not.toHaveBeenCalled();
    track.getHeight();
    track.onMouseClick(event);
    expect(onSliceClick).toHaveBeenCalledOnce();
  });

  test('expanded rows retain normal click behavior', () => {
    const {track, selectTrackEvent} = setup(false);
    track.onMouseClick({x: 50, y: 30, timescale});
    expect(selectTrackEvent).toHaveBeenCalledWith('test', 1);
    expect(track.getHeight()).toBe(42);
  });
});

describe('SliceTrack with a dynamic dataset', () => {
  const CellType = protos.QueryResult.CellsBatch.CellType;

  // Builds a query result with the given integer rows.
  function queryResult(
    columnNames: readonly string[],
    rows: ReadonlyArray<readonly number[]> = [],
  ): QueryResult {
    const batch = protos.QueryResult.CellsBatch.create({
      cells: rows.flatMap((row) => row.map(() => CellType.CELL_VARINT)),
      varintCells: rows.flat(),
      isLastBatch: true,
    });
    const result = createQueryResult({query: ''});
    result.appendResultBatch(
      protos.QueryResult.encode(
        protos.QueryResult.create({
          columnNames: [...columnNames],
          batch: [batch],
        }),
      ).finish(),
    );
    return result;
  }

  // Records all queries and answers the ones issued by SliceTrack: every
  // source holds one slice at depth 0.
  class FakeEngine {
    readonly log: string[] = [];

    // How long building a table takes, to span several frames.
    createDelayMs = 0;

    // Queries that used a table after it was dropped.
    readonly useAfterDrop: string[] = [];
    private readonly dropped = new Set<string>();

    async query(sql: string): Promise<QueryResult> {
      this.log.push(sql);
      for (const name of this.dropped) {
        if (new RegExp(`\\b${name}\\b`).test(sql)) this.useAfterDrop.push(sql);
      }
      if (sql.startsWith('DROP TABLE')) {
        this.dropped.add(sql.split(' ').at(-1)!);
      }
      if (sql.startsWith('CREATE') && this.createDelayMs > 0) {
        await new Promise((resolve) => setTimeout(resolve, this.createDelayMs));
      }
      const schemaColumns = ['id', 'ts', 'dur', 'depth'];
      if (sql.includes('AS rowCount')) {
        return queryResult(['rowCount'], [[1]]);
      }
      if (sql.includes('as __start')) {
        return queryResult(
          [
            ...['__start', '__end', '__id', '__count', '__depth'],
            ...['__incomplete', ...schemaColumns],
          ],
          [[0, 100, 1, 1, 0, 0, 1, 0, 100, 0]],
        );
      }
      if (sql.includes('as __ts')) {
        return queryResult([
          '__id',
          '__ts',
          '__count',
          '__depth',
          ...schemaColumns,
        ]);
      }
      return queryResult([]);
    }

    async tryQuery(sql: string) {
      return okResult(await this.query(sql));
    }

    createdTables(): string[] {
      return this.log
        .filter((q) => q.startsWith('CREATE'))
        .map((q) => q.split(' ')[3]!);
    }

    droppedTables(): string[] {
      return this.log
        .filter((q) => q.startsWith('DROP TABLE'))
        .map((q) => q.split(' ')[4]!);
    }
  }

  function setup() {
    const engine = new FakeEngine();
    const trace = {
      engine,
      taskTracker: {track: <T>(p: Promise<T>) => p},
      raf: {scheduleFullRedraw: vi.fn()},
      selection: {selection: {kind: 'empty'}},
      timeline: {},
    } as unknown as Trace;
    const drawSlices = vi.fn();
    const visibleWindow = new HighPrecisionTimeSpan(
      new HighPrecisionTime(Time.ZERO),
      100,
    );

    // A canvas context whose methods are no-ops.
    const ctx = new Proxy(
      {},
      {
        get: (_, prop) =>
          prop === 'measureText' ? () => ({width: 0}) : () => {},
        set: () => true,
      },
    );

    const renderCtx = {
      visibleWindow,
      size: {width: 100, height: 40},
      resolution: 1n,
      ctx,
      timescale: new TimeScale(visibleWindow, {left: 0, right: 100}),
      colors: {},
      renderer: {drawSlices, drawMarkers: vi.fn()},
    } as unknown as TrackRenderContext;

    return {engine, trace, drawSlices, renderCtx};
  }

  // Renders the track a few times, letting its async tasks run in between.
  async function renderUntilSettled(
    track: TrackRenderer,
    renderCtx: TrackRenderContext,
  ) {
    for (let i = 0; i < 10; i++) {
      track.render(renderCtx);
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
  }

  test('keeps drawing while the tables are rebuilt', async () => {
    const {engine, trace, drawSlices, renderCtx} = setup();
    let src = 'slices_a';
    const track = SliceTrack.create({
      trace,
      uri: 'test',
      dataset: () =>
        new SourceDataset({
          src,
          schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM},
        }),
    });
    const drawnSlices = () => drawSlices.mock.lastCall?.[0].count;

    await renderUntilSettled(track, renderCtx);
    const oldTables = engine.createdTables();
    expect(oldTables).toHaveLength(3);
    expect(drawnSlices()).toBe(1);

    // Every frame draws the previous slices until the new ones are loaded.
    engine.createDelayMs = 5;
    src = 'slices_b';
    for (let i = 0; i < 10; i++) {
      drawSlices.mockClear();
      track.render(renderCtx);
      expect(drawnSlices()).toBe(1);
      await new Promise((resolve) => setTimeout(resolve, 5));
    }

    // The old tables are dropped and slices are loaded from the new ones.
    await renderUntilSettled(track, renderCtx);
    const newTables = engine.createdTables().slice(3);
    expect(newTables).toHaveLength(3);
    expect(engine.droppedTables()).toEqual(oldTables);
    expect(engine.useAfterDrop).toEqual([]);
    const sliceQueries = engine.log.filter((q) => q.includes('as __start'));
    expect(sliceQueries.at(-1)).toContain(newTables[0]);
    expect(sliceQueries.at(-1)).toContain('slices_b');
    expect(drawnSlices()).toBe(1);
  });

  test('draws a dataset that changes on every frame', async () => {
    const {engine, trace, drawSlices, renderCtx} = setup();
    let generation = 0;
    const track = SliceTrack.create({
      trace,
      uri: 'test',
      dataset: () =>
        new SourceDataset({
          src: `slices_${generation++}`,
          schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM},
        }),
    });

    // Each dataset is drawn once loaded, then the latest one is loaded.
    await renderUntilSettled(track, renderCtx);
    expect(drawSlices).toHaveBeenLastCalledWith(
      expect.objectContaining({count: 1}),
      expect.anything(),
      expect.anything(),
    );
    expect(engine.createdTables().length).toBeGreaterThan(3);
    expect(engine.useAfterDrop).toEqual([]);
  });
});
