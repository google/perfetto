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
  GraphLayer,
  GraphNode,
  GraphTooltip,
} from '../../components/widgets/charts/graph_chart';
import {
  type CollapsedEdge,
  GRAPH_LEFT_MARGIN_X,
  GRAPH_ROW_LABEL_X,
  type GraphSnapshot,
  type ProcessGraphModel,
  type ProcessInfo,
  stateStyle,
  yForRank,
} from './model';

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
    ...edge.components.slice(0, 3),
  ];
}

export function buildProcessGraphChartData(
  model: ProcessGraphModel,
  snap: GraphSnapshot,
  selected: SelectedItem | undefined,
): GraphChartData {
  const nodeByUpid = new Map(snap.nodes.map((n) => [n.upid, n]));
  const connectedUpids = new Set<number>();
  for (const e of snap.edges) {
    connectedUpids.add(e.client);
    connectedUpids.add(e.server);
  }

  const rowCounts = new Map<number, number>();
  for (const n of snap.nodes) {
    rowCounts.set(n.stateRank, (rowCounts.get(n.stateRank) ?? 0) + 1);
  }

  const layers: GraphLayer[] = model.globalRanks.map((rank) => {
    const stName = model.globalRankToState.get(rank) ?? 'UNKNOWN';
    const st = stateStyle(stName);
    const cnt = rowCounts.get(rank) ?? 0;
    return {
      id: rank,
      label: `${st.short} (${cnt})`,
      y: yForRank(model, rank),
      color: st.fill,
      lightTextColor: st.lightText,
      dimmed: cnt === 0,
    };
  });

  const xLookup = snap.useTraceColumns ? model.traceConnX : model.fullX;

  const nodes: GraphNode[] = snap.nodes.map((n) => {
    const isSel = selected?.type === 'node' && selected.upid === n.upid;
    const hasEdges = connectedUpids.has(n.upid);
    const tooltip: GraphTooltip = [
      {value: `${n.name} (pid ${n.pid})`, bold: true},
      {label: 'state', value: n.state, bold: true},
      ...(n.reason ? [{label: 'reason', value: n.reason}] : []),
    ];

    return {
      id: String(n.upid),
      label: n.name || `pid ${n.pid}`,
      layer: n.stateRank,
      x: xLookup.get(n.upid) ?? model.fullX.get(n.upid) ?? 500,
      y: yForRank(model, n.stateRank),
      color: stateStyle(n.state).fill,
      opacity: snap.useTraceColumns && !hasEdges && !isSel ? 0.42 : 1,
      selected: isSel,
      tooltip,
    };
  });

  const edges: GraphEdge[] = snap.edges.map((e) => {
    const isSelf = e.client === e.server;
    const clientState = nodeByUpid.get(e.client)?.state ?? 'UNKNOWN';
    const badge = isSelf
      ? `[SELF-LOOP · ${e.kind.toUpperCase()}]`
      : `[${e.kind.toUpperCase()}]`;

    return {
      id: e.key,
      source: String(e.client),
      target: String(e.server),
      weight: e.count,
      color: stateStyle(clientState).fill,
      lineType: e.kind === 'provider' ? 'dashed' : 'solid',
      selected: selected?.type === 'edge' && selected.key === e.key,
      tooltip: buildEdgeTooltip(model, e, badge),
    };
  });

  return {
    layers,
    nodes,
    edges,
    bounds: {
      minX: GRAPH_ROW_LABEL_X,
      maxX: GRAPH_LEFT_MARGIN_X + model.canvasW,
      minY: 0,
      maxY: model.canvasH + 60,
    },
  };
}
