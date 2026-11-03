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
import type {Engine} from '../../trace_processor/engine';
import {computeAxisTicks, formatTick, niceTicks} from './axis';
import {
  buildColorEncoding,
  legendEntries,
  parseCssColor,
  type ScatterTheme,
} from './color_scale';
import {buildOperatorPointSet, nearest} from './point_set';
import {
  computeViewportRequest,
  ScatterController,
  shouldRequery,
} from './scatter_controller';
import {
  scatterMipmapSubquerySql,
  scatterMipmapVirtualTableSql,
  sqlForRect,
  viewportQuerySql,
} from './sql_utils';
import type {ColorMode} from './types';
import {clampView, fitView, panView, zoomView} from './view';

describe('ScatterPlot core utilities and controller', () => {
  test('sql_utils builds escaped virtual table and viewport queries', () => {
    expect(
      scatterMipmapVirtualTableSql('vt_1', 'slice', 'id', 'ts', 'dur', 'depth'),
    ).toBe(
      'CREATE VIRTUAL TABLE "vt_1" USING __intrinsic_scatter_mipmap("slice", "id", "ts", "dur", "depth");',
    );
    expect(
      scatterMipmapSubquerySql(
        'vt_2',
        {kind: 'query', sql: 'SELECT ts, dur FROM slice;'},
        {x: 'ts', y: 'dur'},
      ),
    ).toBe(
      'CREATE VIRTUAL TABLE "vt_2" USING __intrinsic_scatter_mipmap((SELECT NULL AS id, "ts" AS x, "dur" AS y, NULL AS c FROM (SELECT ts, dur FROM slice)));',
    );
    expect(
      viewportQuerySql('vt_1', {
        xMin: 0,
        xMax: 100,
        yMin: 10,
        yMax: 50,
        cols: 64,
        rows: 32,
      }),
    ).toBe('SELECT id, x, y, c, count FROM "vt_1"(0, 100, 10, 50, 64, 32);');
    expect(
      viewportQuerySql('vt_1', {
        xMin: 0,
        xMax: 100,
        yMin: 10,
        yMax: 50,
        cols: 64,
        rows: 32,
        hidden: [-1, 2, 5],
      }),
    ).toBe(
      `SELECT id, x, y, c, count FROM "vt_1"(0, 100, 10, 50, 64, 32, '-1,2,5');`,
    );
    expect(
      sqlForRect(
        {kind: 'table', table: 'slice'},
        {x: 'ts', y: 'dur'},
        {xMin: 10, xMax: 20, yMin: 5, yMax: 15},
      ),
    ).toBe(
      'SELECT * FROM "slice" WHERE "ts" BETWEEN 10 AND 20 AND "dur" BETWEEN 5 AND 15;',
    );
  });

  test('view math: fitView, zoomView, panView, clampView', () => {
    const bounds = {xMin: 100, xMax: 300, yMin: 0, yMax: 100};
    const fitted = fitView(bounds, 0.05);
    expect(fitted).toEqual({x0: 90, x1: 310, y0: -5, y1: 105});

    const zoomed = zoomView({x0: 0, x1: 100, y0: 0, y1: 100}, 2, 2, 50, 50);
    expect(zoomed).toEqual({x0: 25, x1: 75, y0: 25, y1: 75});

    const panned = panView(zoomed, 10, -5);
    expect(panned).toEqual({x0: 35, x1: 85, y0: 20, y1: 70});

    const clamped = clampView({x0: 150, x1: 150, y0: 50, y1: 50}, bounds, 0.01);
    expect(clamped.x1 - clamped.x0).toBeCloseTo(2);
    expect(clamped.y1 - clamped.y0).toBeCloseTo(1);
  });

  test('axis ticks and formatting', () => {
    const ticks = niceTicks(0, 100, 5);
    expect(ticks).toEqual([0, 20, 40, 60, 80, 100]);
    expect(formatTick(1500, 500)).toBe('1.5 K');

    const huge = computeAxisTicks(1e12 + 10, 1e12 + 50, 4);
    expect(huge.offset).toBe(1e12);
    expect(huge.labels[0]).toBe('10');
  });

  test('point_set nearest picking and color encoding', () => {
    const ps = buildOperatorPointSet(
      {
        x: new Float64Array([10, 50, 90]),
        y: new Float64Array([20, 60, 80]),
        row: new Float64Array([1, 2, 3]),
        color: new Float32Array([0, 1, -1]),
        weight: new Float32Array([1, 5, 2]),
      },
      {xMin: 0, xMax: 100, yMin: 0, yMax: 100},
    );
    const hit = nearest(ps, 51, 59, 1, 1, 5);
    expect(hit?.index).toBe(1);
    expect(nearest(ps, 0, 0, 1, 1, 5)).toBeUndefined();

    expect(Array.from(parseCssColor('#4285f4'))).toEqual([66, 133, 244, 255]);
    const enc = buildColorEncoding(
      {
        kind: 'categorical',
        column: 'cat',
        categories: ['A', 'B'],
        counts: [10, 20],
        hasOther: true,
        otherCount: 5,
        nullCount: 0,
      },
      {
        palette: [new Uint8Array([255, 0, 0, 255])],
        uniform: new Uint8Array([0, 0, 255, 255]),
        nullColor: new Uint8Array([128, 128, 128, 255]),
        background: new Uint8Array([255, 255, 255, 255]),
        text: '#000',
        grid: '#ccc',
      },
      new Set([1]),
    );
    expect(enc.lut[3]).toBe(255); // Category 0 visible
    expect(enc.lut[7]).toBe(0); // Category 1 hidden
  });

  test('controller loads source, quantizes viewport, and skips redundant queries', async () => {
    const fetchViewport = vi.fn(async () => ({
      x: new Float64Array([10, 20]),
      y: new Float64Array([30, 40]),
      row: new Float64Array([100, 200]),
      color: new Float32Array([0, 0]),
      count: new Float64Array([1, 3]),
    }));

    const bounds = {xMin: 0, xMax: 1000, yMin: 0, yMax: 500};
    const ctrl = new ScatterController({
      engine: {} as Engine,
      uid: 't',
      onChange: () => {},
      viewportDebounceMs: 0,
      sourceFactory: () => ({
        describe: async () => [
          {name: 'id', kind: 'numeric'},
          {name: 'ts', kind: 'numeric'},
          {name: 'dur', kind: 'numeric'},
        ],
        prepare: async () => ({
          count: 2,
          bounds,
          colorMode: {kind: 'none'},
          fetchViewport,
          fetchRow: async (id: number) => [{name: 'id', value: id}],
          countInRect: async () => 2,
          sqlForRect: () => 'SELECT * FROM "slice";',
        }),
        dispose: async () => {},
        [Symbol.asyncDispose]: async () => {},
      }),
    });

    await ctrl.setSource({kind: 'table', table: 'slice'});
    expect(ctrl.state.kind).toBe('ready');
    expect(ctrl.points?.count).toBe(2);
    expect(fetchViewport).toHaveBeenCalledTimes(1);

    // Small pan inside padded region at same LOD level should not re-query
    const req = computeViewportRequest(ctrl.view!, bounds, 800, 600, 3);
    expect(shouldRequery(ctrl.view!, bounds, req, req)).toBe(false);

    // Hiding a category re-queries with it excluded server-side (cells whose
    // representative was hidden may still contain visible points); showing it
    // again re-queries without the filter.
    const lastHidden = () => {
      const calls = fetchViewport.mock.calls as unknown as Array<
        [{hidden?: ReadonlyArray<number>}]
      >;
      return calls[calls.length - 1][0].hidden;
    };
    ctrl.toggleCategory(2);
    await Promise.resolve();
    expect(fetchViewport).toHaveBeenCalledTimes(2);
    expect(lastHidden()).toEqual([2]);
    ctrl.isolateCategory(0, [0, 1, 2]);
    await Promise.resolve();
    expect(fetchViewport).toHaveBeenCalledTimes(3);
    expect(lastHidden()).toEqual([1, 2]);
    ctrl.showAllCategories();
    await Promise.resolve();
    expect(fetchViewport).toHaveBeenCalledTimes(4);
    expect(lastHidden()).toEqual([]);

    await ctrl.dispose();
  });

  test('legend entries are sorted by count with Other/NULL last', () => {
    const theme: ScatterTheme = {
      palette: [new Uint8Array([255, 0, 0, 255])],
      uniform: new Uint8Array([0, 0, 255, 255]),
      nullColor: new Uint8Array([128, 128, 128, 255]),
      background: new Uint8Array([255, 255, 255, 255]),
      text: '#000',
      grid: '#ccc',
    };
    const mode: ColorMode = {
      kind: 'categorical',
      column: 'name',
      categories: ['a', 'b', 'c', 'd'],
      counts: [5, 50, 5, 7],
      hasOther: true,
      otherCount: 100,
      nullCount: 3,
    };
    const entries = legendEntries(
      mode,
      buildColorEncoding(mode, theme, new Set()),
    );
    expect(entries.map((e) => [e.label, e.count])).toEqual([
      ['b', 50],
      ['d', 7],
      ['a', 5],
      ['c', 5],
      ['Other', 100],
      ['NULL', 3],
    ]);
    expect(entries[4].index).toBe(4);
    expect(entries[5].index).toBe(-1);

    // No NULL rows -> no NULL entry; no Other -> no Other entry.
    const noNull: ColorMode = {...mode, hasOther: false, nullCount: 0};
    expect(
      legendEntries(noNull, buildColorEncoding(noNull, theme, new Set())).map(
        (e) => e.label,
      ),
    ).toEqual(['b', 'd', 'a', 'c']);
  });

  test('nearest skips hidden points', () => {
    const ps = buildOperatorPointSet(
      {
        x: new Float64Array([50, 52]),
        y: new Float64Array([50, 50]),
        row: new Float64Array([1, 2]),
        color: new Float32Array([0, 1]),
      },
      {xMin: 0, xMax: 100, yMin: 0, yMax: 100},
    );
    expect(nearest(ps, 50, 50, 1, 1, 5)?.index).toBe(0);
    expect(nearest(ps, 50, 50, 1, 1, 5, (i) => i === 0)?.index).toBe(1);
    expect(nearest(ps, 50, 50, 1, 1, 5, () => true)).toBeUndefined();
  });

  test('legend toggle / isolate / show all', () => {
    let changes = 0;
    const ctrl = new ScatterController({
      engine: {} as Engine,
      uid: 't',
      onChange: () => changes++,
    });
    const all = [1, 0, 2, 3, -1];
    const hidden = () => [...ctrl.hiddenCategories].sort((a, b) => a - b);

    ctrl.toggleCategory(2);
    expect(hidden()).toEqual([2]);
    ctrl.toggleCategory(2);
    expect(hidden()).toEqual([]);

    // A double-click is delivered as click(detail=1) + click(detail=2): the
    // view toggles on the first and isolates on the second. Net effect:
    // only the clicked entry stays visible.
    ctrl.toggleCategory(0);
    ctrl.isolateCategory(0, all);
    expect(hidden()).toEqual([-1, 1, 2, 3]);

    // Double-clicking the isolated entry again restores everything.
    ctrl.toggleCategory(0);
    ctrl.isolateCategory(0, all);
    expect(hidden()).toEqual([]);

    // Double-clicking a hidden entry isolates it too.
    ctrl.toggleCategory(3);
    ctrl.toggleCategory(3);
    ctrl.isolateCategory(3, all);
    expect(hidden()).toEqual([-1, 0, 1, 2]);

    // Double-clicking a different entry while one is isolated switches.
    ctrl.toggleCategory(1);
    ctrl.isolateCategory(1, all);
    expect(hidden()).toEqual([-1, 0, 2, 3]);

    ctrl.showAllCategories();
    expect(hidden()).toEqual([]);
    const before = changes;
    ctrl.showAllCategories(); // no-op
    expect(changes).toBe(before);
  });
});
