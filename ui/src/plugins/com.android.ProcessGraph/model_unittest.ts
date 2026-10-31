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

import {Time} from '../../base/time';
import {buildGraphChartOption} from '../../components/widgets/charts/graph_chart';
import {buildProcessGraphChartData} from './chart_option';
import {
  buildProcessGraphModel,
  type ProcessGraphFilters,
  snapshotAt,
  stepTimestamp,
  yForRank,
} from './model';
import {formatRelTime} from './process_graph_panel';

const DEFAULT_FILTERS: ProcessGraphFilters = {
  showServices: true,
  showProviders: true,
  showSelfLoops: false,
  showIsolated: false,
  searchQuery: '',
};

function makeFixtureModel() {
  const t = (ns: bigint) => Time.fromRaw(ns);
  return buildProcessGraphModel({
    traceStart: t(1_000n),
    traceEnd: t(10_000n),
    processes: [
      {upid: 1, pid: 100, name: 'system_server', uid: 1000},
      {upid: 2, pid: 200, name: 'com.example.alpha', uid: 10101},
      {upid: 3, pid: 300, name: 'com.example.beta', uid: 10102},
      {upid: 4, pid: 400, name: 'com.example.isolated', uid: 10103},
    ],
    stateIntervals: [
      {
        upid: 1,
        ts: t(1_000n),
        endTs: t(10_001n),
        state: 'PERSISTENT',
        stateRank: 0,
      },
      {
        upid: 2,
        ts: t(1_000n),
        endTs: t(5_000n),
        state: 'TOP',
        stateRank: 2,
      },
      {
        upid: 2,
        ts: t(5_000n),
        endTs: t(10_001n),
        state: 'CACHED_ACTIVITY',
        stateRank: 16,
      },
      {
        upid: 3,
        ts: t(1_000n),
        endTs: t(10_001n),
        state: 'SERVICE',
        stateRank: 10,
      },
      {
        upid: 4,
        ts: t(1_000n),
        endTs: t(10_001n),
        state: 'CACHED_EMPTY',
        stateRank: 19,
      },
    ],
    connections: [
      {
        kind: 'service',
        ts: t(2_000n),
        endTs: t(6_000n),
        clientUpid: 2,
        serverUpid: 3,
        label: 'com.example.beta/.WorkService',
      },
      {
        kind: 'provider',
        ts: t(3_000n),
        endTs: t(10_001n),
        clientUpid: 1,
        serverUpid: 2,
        label: 'com.example.alpha.provider',
      },
    ],
  });
}

describe('ProcessGraph model', () => {
  test('assigns distinct permanent X columns to connected processes', () => {
    const model = makeFixtureModel();
    const x1 = model.traceConnX.get(1);
    const x2 = model.traceConnX.get(2);
    const x3 = model.traceConnX.get(3);
    expect(x1).toBeDefined();
    expect(x2).toBeDefined();
    expect(x3).toBeDefined();
    expect(new Set([x1, x2, x3]).size).toBe(3);
    expect(model.traceConnX.has(4)).toBe(false);

    const sBefore = snapshotAt(model, Time.fromRaw(4_000n), DEFAULT_FILTERS);
    const sAfter = snapshotAt(model, Time.fromRaw(5_500n), DEFAULT_FILTERS);
    const nBefore = sBefore.nodes.find((n) => n.upid === 2);
    const nAfter = sAfter.nodes.find((n) => n.upid === 2);
    expect(nBefore?.state).toBe('TOP');
    expect(nAfter?.state).toBe('CACHED_ACTIVITY');
    expect(yForRank(model, nAfter?.stateRank ?? 0)).toBeGreaterThan(
      yForRank(model, nBefore?.stateRank ?? 0),
    );
    expect(model.traceConnX.get(2)).toBe(x2);
  });

  test('evaluates half-open connection intervals and steps between changes', () => {
    const model = makeFixtureModel();
    const sStart = snapshotAt(model, Time.fromRaw(2_000n), DEFAULT_FILTERS);
    expect(sStart.edges.map((e) => e.key)).toEqual(['service:2>3']);

    const nextTs = stepTimestamp(
      model,
      Time.fromRaw(2_000n),
      1,
      'edge',
      DEFAULT_FILTERS,
    );
    expect(nextTs).toBe(Time.fromRaw(3_000n));
    const sMid = snapshotAt(model, Time.fromRaw(3_000n), DEFAULT_FILTERS);
    expect(sMid.edges.map((e) => e.key)).toEqual([
      'service:2>3',
      'provider:1>2',
    ]);

    const nextEndTs = stepTimestamp(
      model,
      Time.fromRaw(3_000n),
      1,
      'edge',
      DEFAULT_FILTERS,
    );
    expect(nextEndTs).toBe(Time.fromRaw(6_000n));
    const sEnd = snapshotAt(model, Time.fromRaw(6_000n), DEFAULT_FILTERS);
    expect(sEnd.edges.map((e) => e.key)).toEqual(['provider:1>2']);
  });

  test('formats relative durations cleanly across short and multi-hour traces', () => {
    expect(formatRelTime(12_340_000_000n)).toBe('12.34s');
    expect(formatRelTime(65_000_000_000n)).toBe('1m 5s');
    expect(formatRelTime(43_448_000_000_000n)).toBe('12h 4m 8s');
  });

  test('maps snapshots and self-loops into GraphChartData', () => {
    const t = (ns: bigint) => Time.fromRaw(ns);
    const model = buildProcessGraphModel({
      traceStart: t(1_000n),
      traceEnd: t(5_000n),
      processes: [{upid: 1, pid: 100, name: 'com.example.self'}],
      stateIntervals: [
        {upid: 1, ts: t(1_000n), endTs: t(5_001n), state: 'TOP', stateRank: 2},
      ],
      connections: [
        {
          kind: 'service',
          ts: t(1_000n),
          endTs: t(5_001n),
          clientUpid: 1,
          serverUpid: 1,
          label: 'com.example.self/.LoopService',
        },
      ],
    });
    const snap = snapshotAt(model, t(2_000n), {
      ...DEFAULT_FILTERS,
      showSelfLoops: true,
    });
    const data = buildProcessGraphChartData(model, snap, undefined);
    expect(data.layers?.map((l) => l.label)).toEqual(['TOP (1)']);
    const opt = buildGraphChartOption(data, {
      textColor: '#111111',
      borderColor: '#cccccc',
      backgroundColor: '#ffffff',
      accentColor: '#2563eb',
      fontFamily: 'sans-serif',
      chartColors: ['#2563eb'],
    });
    const series = (
      opt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0];
    const loopBadge = series?.data.find(
      (d) => d.id === 'loop-badge:service:1>1',
    );
    expect(loopBadge?.edgeId).toBe('service:1>1');
    expect(String(loopBadge?.symbol)).toContain('path://M ');
  });
});
