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

import type {
  GraphChartData,
  GraphEdge,
  GraphEdgeHighlight,
  GraphLayer,
  GraphNode,
  GraphNodeHighlight,
  GraphTooltip,
} from '../../components/widgets/charts/graph_chart';
import {
  type CollapsedEdge,
  type ContextualSubgraph,
  type GraphSnapshot,
  type ProcessGraphModel,
  type ProcessInfo,
  stateStyle,
} from './model';

export const SNOWFLAKE_SYMBOL_PATH =
  'path://M 10.68 7.39 L 10.89 5.70 L 8.72 2.98 L 10.93 4.37 L 12.00 0.60 ' +
  'L 13.07 4.37 L 15.28 2.98 L 13.11 5.70 L 13.32 7.39 L 15.33 8.55 ' +
  'L 16.90 7.89 L 18.17 4.65 L 18.07 7.26 L 21.87 6.30 L 19.14 9.12 ' +
  'L 21.45 10.33 L 18.01 9.81 L 16.66 10.84 L 16.66 13.16 L 18.01 14.19 ' +
  'L 21.45 13.67 L 19.14 14.88 L 21.87 17.70 L 18.07 16.74 L 18.17 19.35 ' +
  'L 16.90 16.11 L 15.33 15.45 L 13.32 16.61 L 13.11 18.30 L 15.28 21.02 ' +
  'L 13.07 19.63 L 12.00 23.40 L 10.93 19.63 L 8.72 21.02 L 10.89 18.30 ' +
  'L 10.68 16.61 L 8.67 15.45 L 7.10 16.11 L 5.83 19.35 L 5.93 16.74 ' +
  'L 2.13 17.70 L 4.86 14.88 L 2.55 13.67 L 5.99 14.19 L 7.34 13.16 ' +
  'L 7.34 10.84 L 5.99 9.81 L 2.55 10.33 L 4.86 9.12 L 2.13 6.30 ' +
  'L 5.93 7.26 L 5.83 4.65 L 7.10 7.89 L 8.67 8.55 Z';

export type SelectedItem =
  | {readonly type: 'node'; readonly upid: number}
  | {readonly type: 'edge'; readonly key: string};

export function buildProcessInfoFallback(
  model: ProcessGraphModel,
  upid: number,
): ProcessInfo {
  return (
    model.procsByUpid.get(upid) ?? {
      upid,
      pid: 0,
      name: `upid ${upid}`,
    }
  );
}

function buildEdgeTooltip(
  model: ProcessGraphModel,
  edge: CollapsedEdge,
  badge: string,
  stepDiff?: string,
): GraphTooltip {
  const cn = buildProcessInfoFallback(model, edge.client);
  const sn = buildProcessInfoFallback(model, edge.server);
  return [
    {value: badge, bold: true},
    `${cn.name} (pid ${cn.pid}) → ${sn.name} (pid ${sn.pid})`,
    {
      label: 'kind',
      value: `${edge.kind} · connections=${edge.count}`,
      bold: true,
    },
    ...(edge.waivesPriority === true
      ? [{label: 'priority', value: 'waived (NOT_FOREGROUND / WAIVE_PRIORITY)'}]
      : []),
    ...(stepDiff !== undefined
      ? [{label: 'step diff', value: stepDiff, bold: true}]
      : []),
    ...edge.components.slice(0, 3),
  ];
}

export function buildProcessGraphChartData(
  model: ProcessGraphModel,
  subgraph: ContextualSubgraph,
  snap: GraphSnapshot,
  selected: SelectedItem | undefined,
  prevSnap?: GraphSnapshot,
): GraphChartData {
  const colCounts = new Map<number, number>();
  for (const n of snap.nodes) {
    colCounts.set(n.stateRank, (colCounts.get(n.stateRank) ?? 0) + 1);
  }

  const rankSet = new Set<number>(subgraph.orderedRanks);
  for (const rank of colCounts.keys()) {
    rankSet.add(rank);
  }
  const orderedRanks =
    rankSet.size === subgraph.orderedRanks.length
      ? subgraph.orderedRanks
      : [...rankSet].sort((a, b) => b - a);

  const layers: GraphLayer[] = orderedRanks.map((rank) => {
    const stName =
      subgraph.rankToState.get(rank) ??
      snap.nodes.find((n) => n.stateRank === rank)?.state ??
      'UNKNOWN';
    const st = stateStyle(stName);
    const cnt = colCounts.get(rank) ?? 0;
    return {
      id: rank,
      label: `${st.short} (${cnt})`,
      color: st.fill,
      lightTextColor: st.lightText,
      dimmed: cnt === 0,
    };
  });

  const prevVisibleUpids =
    prevSnap !== undefined
      ? new Set(prevSnap.nodes.map((n) => n.upid))
      : undefined;
  const visibleUpids = new Set(snap.nodes.map((n) => n.upid));

  const nodes: GraphNode[] = snap.nodes.map((n) => {
    const isSel = selected?.type === 'node' && selected.upid === n.upid;
    let highlight: GraphNodeHighlight | undefined;
    let diffText: string | undefined;

    if (prevSnap !== undefined && prevVisibleUpids !== undefined) {
      if (!prevVisibleUpids.has(n.upid)) {
        highlight = 'added';
        diffText = 'appeared in view';
      } else {
        const prevNode = prevSnap.allNodesMap.get(n.upid);
        if (prevNode !== undefined) {
          const diffs: string[] = [];
          if (prevNode.state !== n.state) {
            diffs.push(`${prevNode.state} → ${n.state}`);
          }
          if (prevNode.isFrozen !== n.isFrozen) {
            diffs.push(n.isFrozen ? 'UNFROZEN → FROZEN' : 'FROZEN → UNFROZEN');
          }
          if (diffs.length > 0) {
            highlight = 'changed';
            diffText = diffs.join(' · ');
          }
        }
      }
    }

    const tooltip: GraphTooltip = [
      {value: `${n.name} (pid ${n.pid})`, bold: true},
      {label: 'state', value: n.state, bold: true},
      ...(n.isFrozen ? [{label: 'freezer', value: 'FROZEN', bold: true}] : []),
      ...(diffText !== undefined
        ? [{label: 'step diff', value: diffText, bold: true}]
        : []),
      ...(n.reason ? [{label: 'reason', value: n.reason}] : []),
      ...(n.isTarget ? ['Selected on timeline'] : []),
    ];

    return {
      id: String(n.upid),
      label: n.name || `pid ${n.pid}`,
      layer: n.stateRank,
      normPosition: subgraph.normYByUpid.get(n.upid) ?? 0.5,
      color: stateStyle(n.state).fill,
      symbol: n.isFrozen ? SNOWFLAKE_SYMBOL_PATH : undefined,
      symbolKeepAspect: n.isFrozen ? true : undefined,
      symbolSizeScale: n.isFrozen ? 1.68 : undefined,
      borderColor: n.isFrozen ? '#0ea5e9' : undefined,
      borderWidth: n.isFrozen ? 1.45 : undefined,
      accentRingColor: n.isTarget ? '#f59e0b' : undefined,
      labelBold: n.isTarget || isSel,
      showLabel: true,
      selected: isSel,
      highlight,
      tooltip,
    };
  });

  const prevEdgeMap =
    prevSnap !== undefined
      ? new Map(prevSnap.edges.map((pe) => [pe.key, pe]))
      : undefined;
  const curEdgeKeys = new Set<string>();

  const edges: GraphEdge[] = snap.edges.map((e) => {
    curEdgeKeys.add(e.key);
    const isSelf = e.client === e.server;
    const clientState = snap.allNodesMap.get(e.client)?.state ?? 'UNKNOWN';
    const prevEdge = prevEdgeMap?.get(e.key);
    let highlight: GraphEdgeHighlight | undefined;
    let stepDiff: string | undefined;

    if (prevEdgeMap !== undefined) {
      if (prevEdge === undefined) {
        highlight = 'added';
        stepDiff = 'added at this step';
      } else if (e.count > prevEdge.count) {
        highlight = 'added';
        stepDiff = `connections ${prevEdge.count} → ${e.count}`;
      } else if (e.count < prevEdge.count) {
        stepDiff = `connections ${prevEdge.count} → ${e.count}`;
      }
    }

    const baseParts = [
      ...(highlight === 'added' ? ['NEW'] : []),
      ...(isSelf ? ['SELF-LOOP'] : []),
      e.kind.toUpperCase(),
      ...(e.waivesPriority === true ? ['PRIORITY WAIVED'] : []),
    ];
    const badge = `[${baseParts.join(' · ')}]`;

    return {
      id: e.key,
      source: String(e.client),
      target: String(e.server),
      weight: e.count,
      color: stateStyle(clientState).fill,
      lineType: e.kind === 'provider' ? 'dashed' : 'solid',
      width: e.waivesPriority === true ? 0.95 : undefined,
      opacity: e.waivesPriority === true ? 0.3 : undefined,
      selected: selected?.type === 'edge' && selected.key === e.key,
      highlight,
      tooltip: buildEdgeTooltip(model, e, badge, stepDiff),
    };
  });

  if (prevSnap !== undefined) {
    for (const pe of prevSnap.edges) {
      if (
        !curEdgeKeys.has(pe.key) &&
        visibleUpids.has(pe.client) &&
        visibleUpids.has(pe.server)
      ) {
        const isSelf = pe.client === pe.server;
        const badgeParts = [
          'REMOVED',
          ...(isSelf ? ['SELF-LOOP'] : []),
          pe.kind.toUpperCase(),
        ];
        edges.push({
          id: pe.key,
          source: String(pe.client),
          target: String(pe.server),
          weight: pe.count,
          selected: selected?.type === 'edge' && selected.key === pe.key,
          highlight: 'removed',
          tooltip: buildEdgeTooltip(
            model,
            pe,
            `[${badgeParts.join(' · ')}]`,
            'removed at this step',
          ),
        });
      }
    }
  }

  return {
    layers,
    nodes,
    edges,
  };
}
