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
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {
  buildProcessGraphChartData,
  SNOWFLAKE_SYMBOL_PATH,
} from './chart_option';
import {TAG_BOUND_BY_TRACK} from '../com.android.ProcessState/aggregators';
import {resolveProcessGraphContext} from './index';
import {
  buildContextualSubgraph,
  buildProcessGraphModel,
  computeDistinctStepTimes,
  hasPriorityWaivingFlag,
  type ProcessGraphFilters,
  snapshotAt,
  stepTimestamp,
} from './model';
import {formatRelTime, ProcessGraphSession} from './process_graph_panel';

const DEFAULT_FILTERS: ProcessGraphFilters = {
  showServices: true,
  showProviders: true,
  showSelfLoops: false,
  searchQuery: '',
};

const TEST_THEME = {
  textColor: '#111111',
  borderColor: '#cccccc',
  backgroundColor: '#ffffff',
  accentColor: '#2563eb',
  fontFamily: 'sans-serif',
  chartColors: ['#2563eb'],
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
      {upid: 4, pid: 400, name: 'com.example.sibling', uid: 10103},
      {upid: 5, pid: 500, name: 'com.example.unrelated', uid: 10104},
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
      {
        upid: 5,
        ts: t(1_000n),
        endTs: t(10_001n),
        state: 'CACHED_EMPTY',
        stateRank: 19,
      },
    ],
    freezerIntervals: [
      {
        upid: 2,
        ts: t(5_500n),
        endTs: t(9_000n),
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
        kind: 'service',
        ts: t(2_000n),
        endTs: t(8_000n),
        clientUpid: 4,
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

describe('ProcessGraph contextual model', () => {
  test('orders visited state columns from CACHED on the left to PERSISTENT on the right', () => {
    const model = makeFixtureModel();
    const sub = buildContextualSubgraph(model, {
      windowStart: Time.fromRaw(2_000n),
      windowEnd: Time.fromRaw(8_000n),
      targetUpids: [2],
      scopeMode: 'component',
      filters: DEFAULT_FILTERS,
    });

    // Ranks: 19 (CACHED_EMPTY) > 16 (CACHED_ACTIVITY) > 10 (SERVICE) > 2 (TOP) > 0 (PERSISTENT).
    expect(sub.orderedRanks).toEqual([19, 16, 10, 2, 0]);
    expect([...sub.subgraphUpids].sort()).toEqual([1, 2, 3, 4]);
  });

  test('distinguishes Full component from Directed chains scope', () => {
    const model = makeFixtureModel();
    // Graph: 1 -> 2 -> 3 <- 4 (upid 4 is a co-caller of 3, not on a directed chain through 2).
    const fullComp = buildContextualSubgraph(model, {
      windowStart: Time.fromRaw(2_000n),
      windowEnd: Time.fromRaw(8_000n),
      targetUpids: [2],
      scopeMode: 'component',
      filters: DEFAULT_FILTERS,
    });
    const chainsOnly = buildContextualSubgraph(model, {
      windowStart: Time.fromRaw(2_000n),
      windowEnd: Time.fromRaw(8_000n),
      targetUpids: [2],
      scopeMode: 'chains',
      filters: DEFAULT_FILTERS,
    });

    expect(fullComp.subgraphUpids.has(4)).toBe(true);
    expect(chainsOnly.subgraphUpids.has(4)).toBe(false);
    expect([...chainsOnly.subgraphUpids].sort()).toEqual([1, 2, 3]);
  });

  test('steps strictly within [windowStart, windowEnd] across edge, state, and freezer changes', () => {
    const model = makeFixtureModel();
    const sub = buildContextualSubgraph(model, {
      windowStart: Time.fromRaw(2_000n),
      windowEnd: Time.fromRaw(7_000n),
      targetUpids: [2],
      scopeMode: 'chains',
      filters: DEFAULT_FILTERS,
    });

    const t1 = stepTimestamp(
      model,
      sub,
      Time.fromRaw(2_000n),
      1,
      'any',
      DEFAULT_FILTERS,
    );
    expect(t1).toBe(Time.fromRaw(3_000n)); // provider 1->2 acquired

    const t2 = stepTimestamp(
      model,
      sub,
      Time.fromRaw(3_000n),
      1,
      'any',
      DEFAULT_FILTERS,
    );
    expect(t2).toBe(Time.fromRaw(5_000n)); // upid 2 TOP -> CACHED_ACTIVITY

    const t3 = stepTimestamp(
      model,
      sub,
      Time.fromRaw(5_000n),
      1,
      'any',
      DEFAULT_FILTERS,
    );
    expect(t3).toBe(Time.fromRaw(5_500n)); // upid 2 enters FROZEN

    const t4 = stepTimestamp(
      model,
      sub,
      Time.fromRaw(5_500n),
      1,
      'any',
      DEFAULT_FILTERS,
    );
    expect(t4).toBe(Time.fromRaw(6_000n)); // service 2->3 removed

    const tEnd = stepTimestamp(
      model,
      sub,
      Time.fromRaw(6_000n),
      1,
      'any',
      DEFAULT_FILTERS,
    );
    expect(tEnd).toBeUndefined(); // freezer end @ 9000 is outside [2000, 7000]
  });

  test('formats relative durations cleanly across short and multi-hour traces', () => {
    expect(formatRelTime(12_340_000_000n)).toBe('12.34s');
    expect(formatRelTime(65_000_000_000n)).toBe('1m 5s');
    expect(formatRelTime(43_448_000_000_000n)).toBe('12h 4m 8s');
  });

  test('maps frozen snowflake symbol, target accent ring, and self-loops into horizontal GraphChartData', () => {
    const t = (ns: bigint) => Time.fromRaw(ns);
    const model = buildProcessGraphModel({
      traceStart: t(1_000n),
      traceEnd: t(6_000n),
      processes: [{upid: 1, pid: 100, name: 'com.example.self'}],
      stateIntervals: [
        {
          upid: 1,
          ts: t(1_000n),
          endTs: t(6_001n),
          state: 'CACHED_EMPTY',
          stateRank: 19,
        },
      ],
      freezerIntervals: [{upid: 1, ts: t(2_000n), endTs: t(5_000n)}],
      connections: [
        {
          kind: 'service',
          ts: t(1_000n),
          endTs: t(6_001n),
          clientUpid: 1,
          serverUpid: 1,
          label: 'com.example.self/.LoopService',
        },
      ],
    });
    const filters = {...DEFAULT_FILTERS, showSelfLoops: true};
    const sub = buildContextualSubgraph(model, {
      windowStart: t(1_000n),
      windowEnd: t(5_000n),
      targetUpids: [1],
      scopeMode: 'component',
      filters,
    });
    const snap = snapshotAt(model, sub, t(3_000n), filters);
    expect(snap.nodes[0]?.isFrozen).toBe(true);
    expect(snap.nodes[0]?.isTarget).toBe(true);

    const data = buildProcessGraphChartData(model, sub, snap, undefined);
    expect(data.layers?.map((l) => l.label)).toEqual(['CACHED_EMPTY (1)']);

    const opt = buildGraphChartOption(data, TEST_THEME, {
      layout: 'layered',
      layerOrientation: 'horizontal',
      viewportSize: {width: 760, height: 280},
    });
    const series = (
      opt.series as Array<{data: Array<Record<string, unknown>>}>
    )[0];
    const node = series?.data.find((d) => d.id === '1');
    const ring = series?.data.find((d) => d.id === 'accent-ring:1');
    const loopBadge = series?.data.find(
      (d) => d.id === 'loop-badge:service:1>1',
    );
    expect(node?.symbol).toBe(SNOWFLAKE_SYMBOL_PATH);
    expect(ring).toBeDefined();
    expect(loopBadge?.edgeId).toBe('service:1>1');
  });

  test('defaults to direct connections, omits disconnected/exited peers at ts, and highlights diffs', () => {
    expect(new ProcessGraphSession().scopeMode).toBe('neighbors');

    const t = (ns: bigint) => Time.fromRaw(ns);
    // Graph: 1 -> 2 -> 3 -> 5, 1 -> 3 (neighbor-to-neighbor for target 2),
    // 4 -> 3, plus transient peer 6 that bound to 2 and exited before t=5000.
    const model = buildProcessGraphModel({
      traceStart: t(1_000n),
      traceEnd: t(10_000n),
      processes: [
        {upid: 1, pid: 100, name: 'system_server'},
        {upid: 2, pid: 200, name: 'com.example.alpha'},
        {upid: 3, pid: 300, name: 'com.example.beta'},
        {upid: 4, pid: 400, name: 'com.example.sibling'},
        {upid: 5, pid: 500, name: 'com.example.leaf'},
        {upid: 6, pid: 600, name: 'com.example.transient'},
      ],
      stateIntervals: [
        {
          upid: 1,
          ts: t(1_000n),
          endTs: t(10_001n),
          state: 'PERSISTENT',
          stateRank: 0,
        },
        {upid: 2, ts: t(1_000n), endTs: t(5_000n), state: 'TOP', stateRank: 2},
        {
          upid: 2,
          ts: t(5_000n),
          endTs: t(10_001n),
          state: 'CACHED_EMPTY',
          stateRank: 19,
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
        {
          upid: 5,
          ts: t(1_000n),
          endTs: t(10_001n),
          state: 'SERVICE',
          stateRank: 10,
        },
        {
          upid: 6,
          ts: t(2_000n),
          endTs: t(3_000n),
          state: 'RECEIVER',
          stateRank: 11,
        },
      ],
      connections: [
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(9_000n),
          clientUpid: 1,
          serverUpid: 2,
          detail: 'flags=AUTO_CREATE,NOT_FOREGROUND',
        },
        {
          kind: 'provider',
          ts: t(2_000n),
          endTs: t(5_000n),
          clientUpid: 1,
          serverUpid: 2,
          label: 'com.example.alpha.provider',
        },
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(9_000n),
          clientUpid: 1,
          serverUpid: 3,
          detail: 'flags=AUTO_CREATE',
        },
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(5_000n),
          clientUpid: 2,
          serverUpid: 3,
          detail: 'flags=AUTO_CREATE',
        },
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(9_000n),
          clientUpid: 3,
          serverUpid: 5,
          detail: 'flags=AUTO_CREATE,WAIVE_PRIORITY',
        },
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(9_000n),
          clientUpid: 4,
          serverUpid: 3,
          detail: 'flags=AUTO_CREATE',
        },
        {
          kind: 'service',
          ts: t(2_000n),
          endTs: t(3_000n),
          clientUpid: 6,
          serverUpid: 2,
          detail: 'flags=AUTO_CREATE',
        },
      ],
    });

    expect(hasPriorityWaivingFlag('AUTO_CREATE,WAIVE_PRIORITY')).toBe(true);
    expect(hasPriorityWaivingFlag('flags=AUTO_CREATE,NOT_FOREGROUND')).toBe(
      true,
    );
    expect(hasPriorityWaivingFlag('AUTO_CREATE,IMPORTANT')).toBe(false);

    const neighborsSub = buildContextualSubgraph(model, {
      windowStart: t(2_000n),
      windowEnd: t(7_000n),
      targetUpids: [2],
      scopeMode: 'neighbors',
      filters: DEFAULT_FILTERS,
    });
    expect([...neighborsSub.subgraphUpids].sort()).toEqual([1, 2, 3, 6]);
    expect(neighborsSub.orderedRanks).not.toContain(21);

    const chainsSub = buildContextualSubgraph(model, {
      windowStart: t(2_000n),
      windowEnd: t(7_000n),
      targetUpids: [2],
      scopeMode: 'chains',
      filters: DEFAULT_FILTERS,
    });
    expect([...chainsSub.subgraphUpids].sort()).toEqual([1, 2, 3, 5, 6]);

    const steps = computeDistinctStepTimes(
      model,
      neighborsSub,
      'any',
      DEFAULT_FILTERS,
    );
    expect(steps).toEqual([t(2_000n), t(3_000n), t(5_000n)]);

    const snapBefore = snapshotAt(
      model,
      neighborsSub,
      t(2_000n),
      DEFAULT_FILTERS,
    );
    // Neighbor-to-neighbor edge 1 -> 3 is excluded in 'neighbors' scope.
    expect(snapBefore.edges.map((e) => e.key).sort()).toEqual([
      'provider:1>2',
      'service:1>2',
      'service:2>3',
      'service:6>2',
    ]);

    const snapMid = snapshotAt(model, neighborsSub, t(3_000n), DEFAULT_FILTERS);
    // Exited transient peer 6 has 0 active edges at t=3000 and is omitted.
    expect(snapMid.nodes.map((n) => n.upid).sort()).toEqual([1, 2, 3]);

    const snapAfter = snapshotAt(
      model,
      neighborsSub,
      t(5_000n),
      DEFAULT_FILTERS,
    );
    // Disconnected peer 3 (still alive in SERVICE) has 0 edges to target 2 at t=5000 and is omitted.
    expect(snapAfter.nodes.map((n) => n.upid).sort()).toEqual([1, 2]);
    // In 'chains' scope at t=5000, 2 -> 3 is broken so 3 -> 5 is also omitted.
    const chainsAfter = snapshotAt(
      model,
      chainsSub,
      t(5_000n),
      DEFAULT_FILTERS,
    );
    expect(chainsAfter.nodes.map((n) => n.upid).sort()).toEqual([1, 2]);

    const waivedEdge = snapAfter.edges.find((e) => e.key === 'service:1>2');
    expect(waivedEdge?.waivesPriority).toBe(true);

    const chartData = buildProcessGraphChartData(
      model,
      neighborsSub,
      snapAfter,
      undefined,
      snapMid,
    );
    const changedNode = chartData.nodes.find((n) => n.id === '2');
    const dimmedEdge = chartData.edges.find((e) => e.id === 'service:1>2');
    const removedEdge = chartData.edges.find((e) => e.id === 'provider:1>2');

    expect(changedNode?.highlight).toBe('changed');
    expect(dimmedEdge?.opacity).toBe(0.3);
    expect(dimmedEdge?.width).toBe(0.95);
    expect(removedEdge?.highlight).toBe('removed');
  });

  test('resolves service and provider peer child track selections without leaking collapsed-group peers', () => {
    const t = (ns: bigint) => Time.fromRaw(ns);
    const rootTrack: Track = {
      uri: 'com.android.ProcessState#process.162.bound_by',
      renderer: {} as Track['renderer'],
      tags: {type: TAG_BOUND_BY_TRACK, upid: 162},
    };
    const peerChildTrack: Track = {
      uri: 'com.android.ProcessState#process.162.bound_by.109',
      renderer: {} as Track['renderer'],
      tags: {type: TAG_BOUND_BY_TRACK, upid: 162},
    };
    const providerPeerTrack: Track = {
      uri: 'com.android.ProcessState#process.162.provider_bound_by.109',
      renderer: {} as Track['renderer'],
      tags: {type: TAG_BOUND_BY_TRACK, upid: 162},
    };
    const tracksByUri = new Map<string, Track>([
      [rootTrack.uri, rootTrack],
      [peerChildTrack.uri, peerChildTrack],
      [providerPeerTrack.uri, providerPeerTrack],
    ]);
    const fakeTrace = {
      traceInfo: {start: t(1_000n), end: t(9_000n)},
      tracks: {getTrack: (uri: string) => tracksByUri.get(uri)},
    } as unknown as Trace;
    const edgeMap = new Map<number, string>([
      [7, 'service:109>162'],
      [8, 'provider:109>162'],
    ]);

    // Area drag over collapsed Bound by (includes root + peer child) keeps only upid 162.
    const collapsedArea = resolveProcessGraphContext(
      fakeTrace,
      {
        kind: 'area',
        start: t(2_000n),
        end: t(5_000n),
        trackUris: [rootTrack.uri, peerChildTrack.uri],
        tracks: [rootTrack, peerChildTrack],
      },
      edgeMap,
    );
    expect(collapsedArea?.targetUpids).toEqual([162]);

    // Area drag directly on service or provider peer child track includes both 109 and 162.
    for (const track of [peerChildTrack, providerPeerTrack]) {
      const peerArea = resolveProcessGraphContext(
        fakeTrace,
        {
          kind: 'area',
          start: t(2_000n),
          end: t(5_000n),
          trackUris: [track.uri],
          tracks: [track],
        },
        edgeMap,
      );
      expect(peerArea?.targetUpids).toEqual([109, 162]);
    }

    // Slice click on provider peer child track preselects the provider edge.
    const sliceClick = resolveProcessGraphContext(
      fakeTrace,
      {
        kind: 'track_event',
        trackUri: providerPeerTrack.uri,
        eventId: 8,
        ts: t(2_000n),
        dur: 1_000n,
      },
      edgeMap,
    );
    expect(sliceClick?.targetUpids).toEqual([162]);
    expect(sliceClick?.preselectedEdgeKey).toBe('provider:109>162');
  });
});
