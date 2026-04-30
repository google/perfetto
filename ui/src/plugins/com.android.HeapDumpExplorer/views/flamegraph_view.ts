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

import m from 'mithril';
import type {Trace} from '../../../public/trace';
import {time} from '../../../base/time';
import {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {
  Flamegraph,
  FlamegraphState,
  FlamegraphOptionalAction,
} from '../../../widgets/flamegraph';

export interface FlamegraphViewAttrs {
  trace: Trace;
  upid: number;
  ts: time;
  state: FlamegraphState | undefined;
  onStateChange: (state: FlamegraphState) => void;
  // Open the flamegraph-objects tab listing rows in the active dump
  // matching `pathHashes` (CSV) for either the class or dominator tree.
  onShowObjects: (pathHashes: string, isDominator: boolean) => void;
}

const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
];

// path_hash_stable rides along on every metric so action handlers can read
// it from `kv` to identify the clicked node. CONCAT_WITH_COMMA matches the
// timeline-side flamegraph's convention; isVisible: false hides it from the
// tooltip — it's only useful programmatically.
const PATH_HASH_AGG_PROP = {
  name: 'path_hash_stable',
  displayName: 'Path Hash',
  mergeAggregation: 'CONCAT_WITH_COMMA' as const,
  isVisible: () => false,
};

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Build a JAVA_HEAP_GRAPH metric for one of the two trees (BFS class tree
// or dominator class tree) selecting a numeric column as the flamegraph
// `value`. Other columns ride along for tooltips and action `kv` lookups.
function buildMetric(
  upid: number,
  ts: time,
  name: string,
  unit: string,
  // The column projected as the flamegraph's `value`. The other one is
  // selected as-is (so it appears in the tooltip but isn't the size).
  valueColumn: 'self_size' | 'self_count',
  isDominator: boolean,
  showObjectsAction: FlamegraphOptionalAction,
): QueryFlamegraphMetric {
  const tree = isDominator
    ? '_heap_graph_dominator_class_tree'
    : '_heap_graph_class_tree';
  const dependencyModule = isDominator
    ? 'android.memory.heap_graph.dominator_class_tree'
    : 'android.memory.heap_graph.class_tree';
  const valueAlias = `${valueColumn} as value`;
  const otherCol = valueColumn === 'self_size' ? 'self_count' : 'self_size';
  return {
    name,
    unit,
    dependencySql: `include perfetto module ${dependencyModule};`,
    statement: `
      select
        id,
        parent_id as parentId,
        ifnull(name, '[Unknown]') as name,
        root_type,
        heap_type,
        ${valueAlias},
        ${otherCol},
        path_hash_stable
      from ${tree}
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties:
      valueColumn === 'self_size'
        ? [SELF_COUNT_AGG_PROP, PATH_HASH_AGG_PROP]
        : [PATH_HASH_AGG_PROP],
    optionalNodeActions: [showObjectsAction],
  };
}

function buildHeapGraphMetrics(
  upid: number,
  ts: time,
  onShowObjects: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<QueryFlamegraphMetric> {
  const showObjectsAction = (
    isDominator: boolean,
  ): FlamegraphOptionalAction => ({
    name: 'Show objects from this class',
    execute: async ({properties}) => {
      const pathHashes = properties.get('path_hash_stable');
      if (pathHashes === undefined) return;
      onShowObjects(pathHashes, isDominator);
    },
  });

  return [
    buildMetric(
      upid,
      ts,
      'Object Size',
      'B',
      'self_size',
      false,
      showObjectsAction(false),
    ),
    buildMetric(
      upid,
      ts,
      'Object Count',
      '',
      'self_count',
      false,
      showObjectsAction(false),
    ),
    buildMetric(
      upid,
      ts,
      'Dominated Object Size',
      'B',
      'self_size',
      true,
      showObjectsAction(true),
    ),
    buildMetric(
      upid,
      ts,
      'Dominated Object Count',
      '',
      'self_count',
      true,
      showObjectsAction(true),
    ),
  ];
}

const FlamegraphView: m.ClosureComponent<FlamegraphViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey = '';

  return {
    view({attrs}) {
      const key = `${attrs.upid}:${attrs.ts}`;
      if (key !== cachedKey) {
        cachedMetrics = buildHeapGraphMetrics(
          attrs.upid,
          attrs.ts,
          attrs.onShowObjects,
        );
        cachedKey = key;
      }
      const metrics = cachedMetrics!;

      // Initialize state on first render or rebase to new metrics if dump
      // changed without the parent reseting state.
      if (
        attrs.state === undefined ||
        !metrics.some((mt) => mt.name === attrs.state!.selectedMetricName)
      ) {
        attrs.onStateChange(
          attrs.state === undefined
            ? Flamegraph.createDefaultState(metrics)
            : Flamegraph.updateState(attrs.state, metrics),
        );
      }

      return m(
        'div',
        {class: 'ah-view-content ah-flamegraph-view'},
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics,
          state: attrs.state,
          onStateChange: attrs.onStateChange,
        }),
      );
    },
  };
};

export default FlamegraphView;
