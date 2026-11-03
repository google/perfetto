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

import type {ChartThemeColors} from './chart_theme';
import {
  buildGraphChartOption,
  computeBarycenterOrder,
  computeKeyboardRoam,
  effectiveNodeScaleRatio,
  escapeGraphTooltipHtml,
  formatGraphTooltip,
} from './graph_chart';

const TEST_THEME: ChartThemeColors = {
  textColor: '#111111',
  borderColor: '#cccccc',
  backgroundColor: '#ffffff',
  accentColor: '#2563eb',
  fontFamily: 'sans-serif',
  chartColors: ['#2563eb'],
};

describe('GraphChart helpers', () => {
  test('computeBarycenterOrder uncrosses a 2-layer bipartite graph', () => {
    const order = computeBarycenterOrder(
      [
        {id: 'topA', layer: 0, tieBreak: 'a'},
        {id: 'topB', layer: 0, tieBreak: 'b'},
        {id: 'botA', layer: 1, tieBreak: 'a'},
        {id: 'botB', layer: 1, tieBreak: 'b'},
      ],
      [
        {source: 'topA', target: 'botB'},
        {source: 'topB', target: 'botA'},
      ],
    );
    const dxTop =
      (order.normX.get('topA') ?? 0) - (order.normX.get('topB') ?? 0);
    const dxBot =
      (order.normX.get('botB') ?? 0) - (order.normX.get('botA') ?? 0);
    expect(Math.sign(dxTop)).toBe(Math.sign(dxBot));
  });

  test('builds layered option with auto coordinates, self-loop badges, and parallel edges', () => {
    const opt = buildGraphChartOption(
      {
        layers: [
          {id: 'high', label: 'HIGH'},
          {id: 'low', label: 'LOW'},
        ],
        nodes: [
          {id: 'n1', label: 'alpha', layer: 'high'},
          {id: 'n2', label: 'beta', layer: 'low'},
        ],
        edges: [
          {id: 'e1', source: 'n1', target: 'n2'},
          {id: 'e2', source: 'n1', target: 'n2'},
          {id: 'loop', source: 'n1', target: 'n1', weight: 2},
        ],
      },
      TEST_THEME,
    );
    const series = (
      opt.series as Array<{
        layout: string;
        data: Array<Record<string, unknown>>;
        links: Array<Record<string, unknown>>;
      }>
    )[0];
    expect(series?.layout).toBe('none');

    const n1 = series?.data.find((d) => d.id === 'n1');
    const n2 = series?.data.find((d) => d.id === 'n2');
    expect(n1?.name).toBe('alpha ↻2');
    expect(Number(n2?.y)).toBeGreaterThan(Number(n1?.y));

    const loopBadge = series?.data.find((d) => d.id === 'loop-badge:loop');
    expect(loopBadge?.edgeId).toBe('loop');
    expect(String(loopBadge?.symbol)).toContain('path://M ');

    const parallelLinks = series?.links.filter(
      (l) => l.source === 'n1' && l.target === 'n2',
    );
    expect(parallelLinks).toHaveLength(2);
    const curvatures = parallelLinks?.map(
      (l) => (l.lineStyle as {curveness: number}).curveness,
    );
    expect(new Set(curvatures).size).toBe(2);
  });

  test('supports circular and force layouts', () => {
    const circularOpt = buildGraphChartOption(
      {
        nodes: [
          {id: 'a', label: 'A'},
          {id: 'b', label: 'B'},
          {id: 'c', label: 'C'},
        ],
        edges: [{id: 'e', source: 'a', target: 'b'}],
      },
      TEST_THEME,
      {layout: 'circular'},
    );
    const circularSeries = (
      circularOpt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0];
    const coords = circularSeries?.data.map((d) => `${d.x},${d.y}`);
    expect(new Set(coords).size).toBe(3);

    const forceOpt = buildGraphChartOption(
      {
        nodes: [{id: 'a', label: 'A'}],
        edges: [],
      },
      TEST_THEME,
      {layout: 'force'},
    );
    const forceSeries = (forceOpt.series as Array<{layout: string}>)[0];
    expect(forceSeries?.layout).toBe('force');
  });

  test('escapes HTML and template braces in tooltips', () => {
    expect(escapeGraphTooltipHtml(`<img src=x onerror="1">{b}&`)).toBe(
      '&lt;img src=x onerror=&quot;1&quot;&gt;&#123;b&#125;&amp;',
    );
    expect(
      formatGraphTooltip(
        [{label: 'node', value: '<b>{a}</b>', bold: true}],
        'fallback',
      ),
    ).toBe('node: <b>&lt;b&gt;&#123;a&#125;&lt;/b&gt;</b>');
  });

  test('maps WASD roam keys and caps node zoom scaling at 1.5x', () => {
    expect(computeKeyboardRoam('KeyW', 'w', false)).toEqual({
      kind: 'pan',
      dx: 0,
      dy: 48,
    });
    expect(computeKeyboardRoam('KeyW', 'W', true)).toEqual({
      kind: 'zoom',
      zoom: 1.18,
    });
    expect(computeKeyboardRoam('KeyQ', 'q', false)).toBeUndefined();
    expect(effectiveNodeScaleRatio(1)).toBe(0.25);
    expect(effectiveNodeScaleRatio(3)).toBe(0.25);
    expect(effectiveNodeScaleRatio(5)).toBeCloseTo(0.125, 4);
  });

  test('separates adjacent same-row nodes from hubs and syncs self-loop drag offsets', () => {
    const spokes = Array.from({length: 60}, (_, i) => ({
      id: `s${i}`,
      label: `spoke-${i}`,
      layer: 'low',
      x: 250 + i * 5,
      y: 120,
    }));
    const data = {
      layers: [
        {id: 'top', label: 'TOP', y: 40},
        {id: 'low', label: 'LOW', y: 120},
      ],
      nodes: [
        {id: 'hub', label: 'hub', layer: 'top', x: 500, y: 40},
        {id: 'neighbor', label: 'neighbor', layer: 'top', x: 503, y: 40},
        ...spokes,
      ],
      edges: [
        ...spokes.map((s, i) => ({id: `e${i}`, source: s.id, target: 'hub'})),
        {id: 'self1', source: 'hub', target: 'hub'},
      ],
      bounds: {minX: 215, maxX: 710, minY: 0, maxY: 200},
    };

    const baseOpt = buildGraphChartOption(data, TEST_THEME, {
      viewportSize: {width: 440, height: 380},
    });
    const baseData = (
      baseOpt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0]?.data;
    const hubIdx = baseData?.findIndex((d) => d.id === 'hub') ?? -1;
    const neighborIdx = baseData?.findIndex((d) => d.id === 'neighbor') ?? -1;
    const hub = baseData?.[hubIdx];
    const neighbor = baseData?.[neighborIdx];
    const badge = baseData?.find((d) => d.id === 'loop-badge:self1');

    expect(Number(hub?.symbolSize)).toBeLessThanOrEqual(22);
    expect(hubIdx).toBeLessThan(neighborIdx);
    const minSep =
      (Number(hub?.symbolSize) + Number(neighbor?.symbolSize)) / 2 + 16;
    expect(Number(neighbor?.x) - Number(hub?.x)).toBeGreaterThanOrEqual(minSep);
    expect((badge?.symbolOffset as number[])[0]).toBeLessThan(0);

    const draggedOpt = buildGraphChartOption(data, TEST_THEME, {
      viewportSize: {width: 440, height: 380},
      dragOffsets: new Map([['hub', {dx: -45, dy: 30}]]),
    });
    const draggedData = (
      draggedOpt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0]?.data;
    const draggedHub = draggedData?.find((d) => d.id === 'hub');
    const draggedBadge = draggedData?.find((d) => d.id === 'loop-badge:self1');

    expect(Number(draggedHub?.x)).toBeCloseTo(Number(hub?.x) - 45, 5);
    expect(Number(draggedHub?.y)).toBeCloseTo(Number(hub?.y) + 30, 5);
    expect(Number(draggedBadge?.x)).toBeCloseTo(Number(draggedHub?.x), 5);
    expect(Number(draggedBadge?.y)).toBeCloseTo(Number(draggedHub?.y), 5);
    expect(draggedBadge?.symbolOffset).toEqual(badge?.symbolOffset);

    const collideDelta = Number(neighbor?.x) - Number(hub?.x) - 2;
    const clampedOpt = buildGraphChartOption(data, TEST_THEME, {
      viewportSize: {width: 440, height: 380},
      dragOffsets: new Map([['hub', {dx: collideDelta, dy: 0}]]),
    });
    const clampedData = (
      clampedOpt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0]?.data;
    const clampedHub = clampedData?.find((d) => d.id === 'hub');
    const clampedNeighbor = clampedData?.find((d) => d.id === 'neighbor');
    const clampedBadge = clampedData?.find((d) => d.id === 'loop-badge:self1');
    expect(
      Math.abs(Number(clampedNeighbor?.x) - Number(clampedHub?.x)),
    ).toBeGreaterThanOrEqual(minSep);
    expect(Number(clampedBadge?.x)).toBeCloseTo(Number(clampedHub?.x), 5);
  });

  test('lays out horizontal columns with custom symbols, accent rings, and vertical separation', () => {
    const customPath = 'path://M 12 2 L 22 12 L 12 22 L 2 12 Z';
    const data = {
      layers: [
        {id: 'left', label: 'LEFT (2)'},
        {id: 'mid', label: 'MID (0)', dimmed: true},
        {id: 'right', label: 'RIGHT (1)'},
      ],
      nodes: [
        {
          id: 'c1',
          label: 'alpha',
          layer: 'left',
          normPosition: 0.5,
          symbol: customPath,
          symbolKeepAspect: true,
          symbolSizeScale: 1.5,
          borderColor: '#38bdf8',
          borderWidth: 1.5,
        },
        {
          id: 'c2',
          label: 'beta',
          layer: 'left',
          normPosition: 0.501,
          accentRingColor: '#f59e0b',
          labelBold: true,
        },
        {
          id: 's1',
          label: 'gamma',
          layer: 'right',
          normPosition: 0.5,
        },
      ],
      edges: [{id: 'e1', source: 'c1', target: 's1'}],
    };

    const opt = buildGraphChartOption(data, TEST_THEME, {
      layout: 'layered',
      layerOrientation: 'horizontal',
      viewportSize: {width: 800, height: 300},
    });
    const series = (
      opt.series as Array<{
        data: Array<Record<string, unknown>>;
        links: Array<Record<string, unknown>>;
      }>
    )[0];

    const colLeft = series?.data.find((d) => d.id === 'col-label:left');
    const colMid = series?.data.find((d) => d.id === 'col-label:mid');
    const colRight = series?.data.find((d) => d.id === 'col-label:right');
    expect(Number(colLeft?.x)).toBeLessThan(Number(colMid?.x));
    expect(Number(colMid?.x)).toBeLessThan(Number(colRight?.x));

    const guideMid = series?.links.find((l) => l.id === 'col-guide:mid');
    expect((guideMid?.lineStyle as {type: string})?.type).toBe('dashed');

    const c1 = series?.data.find((d) => d.id === 'c1');
    const c2 = series?.data.find((d) => d.id === 'c2');
    const s1 = series?.data.find((d) => d.id === 's1');
    const ring = series?.data.find((d) => d.id === 'accent-ring:c2');

    expect(c1?.symbol).toBe(customPath);
    expect(Number(c1?.symbolSize)).toBeGreaterThan(Number(c2?.symbolSize));
    expect(Number(c1?.x)).toBeCloseTo(Number(colLeft?.x), 5);
    expect(Number(s1?.x)).toBeCloseTo(Number(colRight?.x), 5);
    expect(Math.abs(Number(c2?.y) - Number(c1?.y))).toBeGreaterThanOrEqual(28);
    expect(Number(ring?.x)).toBeCloseTo(Number(c2?.x), 5);
    expect(Number(ring?.y)).toBeCloseTo(Number(c2?.y), 5);

    const draggedOpt = buildGraphChartOption(data, TEST_THEME, {
      layout: 'layered',
      layerOrientation: 'horizontal',
      viewportSize: {width: 800, height: 300},
      dragOffsets: new Map([['c2', {dx: 40, dy: -20}]]),
    });
    const draggedData = (
      draggedOpt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0]?.data;
    const draggedC2 = draggedData?.find((d) => d.id === 'c2');
    const draggedRing = draggedData?.find((d) => d.id === 'accent-ring:c2');
    expect(Number(draggedRing?.x)).toBeCloseTo(Number(draggedC2?.x), 5);
    expect(Number(draggedRing?.y)).toBeCloseTo(Number(draggedC2?.y), 5);
  });

  test('compacts dimmed horizontal columns and preserves crisp custom symbol borders on highlight', () => {
    const customPath = 'path://M 12 2 L 22 12 L 12 22 L 2 12 Z';
    const opt = buildGraphChartOption(
      {
        layers: [
          {id: 'a', label: 'A (1)'},
          {id: 'b', label: 'B (1)'},
          {id: 'e1', label: 'E1 (0)', dimmed: true},
          {id: 'e2', label: 'E2 (0)', dimmed: true},
          {id: 'e3', label: 'E3 (0)', dimmed: true},
          {id: 'c', label: 'C (1)'},
        ],
        nodes: [
          {
            id: 'n1',
            label: 'alpha',
            layer: 'a',
            symbol: customPath,
            borderWidth: 1.45,
            highlight: 'changed',
          },
          {id: 'n2', label: 'beta', layer: 'b', highlight: 'changed'},
          {id: 'n3', label: 'gamma', layer: 'c'},
        ],
        edges: [{id: 'edge1', source: 'n1', target: 'n2'}],
      },
      TEST_THEME,
      {
        layout: 'layered',
        layerOrientation: 'horizontal',
        viewportSize: {width: 600, height: 260},
      },
    );
    const data = (
      opt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0]?.data;
    const colX = (id: string) =>
      Number(data?.find((d) => d.id === `col-label:${id}`)?.x);

    const activeGap = colX('b') - colX('a');
    const dimmedGap = colX('e2') - colX('e1');
    expect(dimmedGap).toBeCloseTo(activeGap * 0.4, 4);
    expect(colX('a')).toBeLessThan(colX('b'));
    expect(colX('e3')).toBeLessThan(colX('c'));

    const n1Style = data?.find((d) => d.id === 'n1')?.itemStyle as {
      borderWidth: number;
    };
    const n2Style = data?.find((d) => d.id === 'n2')?.itemStyle as {
      borderWidth: number;
    };
    expect(n1Style.borderWidth).toBe(1.45);
    expect(n2Style.borderWidth).toBe(3);
  });
});
