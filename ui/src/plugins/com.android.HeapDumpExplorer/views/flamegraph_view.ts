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
import type {time} from '../../../base/time';
import type {QueryFlamegraphMetric} from '../../../components/query_flamegraph';
import {FlamegraphPanel} from '../../../components/flamegraph_panel';
import {
  Flamegraph,
  type FlamegraphState,
  type FlamegraphOptionalAction,
} from '../../../widgets/flamegraph';

// Referenced by session.openFlamegraphPivotedAt.
export const METRIC_OBJECT_SIZE = 'Object Size';
export const METRIC_DOMINATED_OBJECT_SIZE = 'Dominated Object Size';

interface FlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: FlamegraphState | undefined;
  readonly onStateChange: (state: FlamegraphState) => void;
  // Open the flamegraph-objects tab for `pathHashes` (CSV).
  readonly onShowObjects: (pathHashes: string, isDominator: boolean) => void;
}

// path_hash_stable is exposed unaggregatable (and CAST to TEXT in SQL,
// since the stdlib emits it as INT64 and the flamegraph reads
// unaggregatable columns as STR_NULL) so it lands in `matchingColumns`
// — that's what lets a PIVOT filter target a specific node by its hash.
// Hidden from the tooltip via `isVisible: false`.
const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
  {
    name: 'path_hash_stable',
    displayName: 'Path Hash',
    isVisible: () => false,
  },
];

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Build a JAVA_HEAP_GRAPH metric for the BFS or dominator class tree,
// projecting `valueColumn` as `value` and the other column for tooltips.
function buildMetric(
  upid: number,
  ts: time,
  name: string,
  unit: string,
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
        ${valueColumn} as value,
        ${otherCol},
        CAST(path_hash_stable AS TEXT) AS path_hash_stable
      from ${tree}
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties:
      valueColumn === 'self_size' ? [SELF_COUNT_AGG_PROP] : [],
    optionalNodeActions: [showObjectsAction],
  };
}

interface MetricSpec {
  readonly name: string;
  readonly unit: string;
  readonly valueColumn: 'self_size' | 'self_count';
  readonly isDominator: boolean;
}

const METRIC_SPECS: ReadonlyArray<MetricSpec> = [
  {
    name: METRIC_OBJECT_SIZE,
    unit: 'B',
    valueColumn: 'self_size',
    isDominator: false,
  },
  {
    name: 'Object Count',
    unit: '',
    valueColumn: 'self_count',
    isDominator: false,
  },
  {
    name: METRIC_DOMINATED_OBJECT_SIZE,
    unit: 'B',
    valueColumn: 'self_size',
    isDominator: true,
  },
  {
    name: 'Dominated Object Count',
    unit: '',
    valueColumn: 'self_count',
    isDominator: true,
  },
];

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
  return METRIC_SPECS.map((s) =>
    buildMetric(
      upid,
      ts,
      s.name,
      s.unit,
      s.valueColumn,
      s.isDominator,
      showObjectsAction(s.isDominator),
    ),
  );
}

const FlamegraphView: m.ClosureComponent<FlamegraphViewAttrs> = () => {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;

  return {
    view({attrs}) {
      const key = `${attrs.upid}:${attrs.ts}`;
      if (cachedMetrics === undefined || key !== cachedKey) {
        cachedMetrics = buildHeapGraphMetrics(
          attrs.upid,
          attrs.ts,
          attrs.onShowObjects,
        );
        cachedKey = key;
      }
      const metrics = cachedMetrics;

      // First render or after a dump-change reset: create a default
      // state so the panel renders meaningfully on the same frame.

      let state = attrs.state;
      if (state === undefined) {
        state = Flamegraph.createDefaultState(metrics);
        attrs.onStateChange(state);
      }

      return m(
        'div',
        {class: 'pf-hde-view-content pf-hde-flamegraph-view'},
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics,
          state,
          onStateChange: attrs.onStateChange,
        }),
      );
    },
  };
};

export default FlamegraphView;
