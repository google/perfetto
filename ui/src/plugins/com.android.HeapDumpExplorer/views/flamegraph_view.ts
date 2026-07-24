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
import {QuerySlot} from '../../../base/query_slot';
import {
  isHeapGraphIncomplete,
  incompleteFlamegraphModal,
} from '../../dev.perfetto.HeapProfile/incomplete_flamegraph';

// Referenced by session.openFlamegraphPivotedOnClass.
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

const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
];

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Must stay aggregatable: as a frame-identity column it would split
// otherwise-identical frames in bottom-up. Merged nodes get the comma-joined
// hash list, read by the "Show objects" drill-down.
const PATH_HASH_AGG_PROP = {
  name: 'path_hash_stable',
  displayName: 'Path Hash',
  mergeAggregation: 'CONCAT_WITH_COMMA' as const,
  isVisible: () => false,
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
        ifnull(name, 'unknown') as name,
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
      valueColumn === 'self_size'
        ? [SELF_COUNT_AGG_PROP, PATH_HASH_AGG_PROP]
        : [PATH_HASH_AGG_PROP],
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
    icon: 'data_object',
    category: 'DRILL',
    description: 'List the individual objects of this class.',
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

export function FlamegraphView(): m.Component<FlamegraphViewAttrs> {
  let cachedMetrics: ReadonlyArray<QueryFlamegraphMetric> | undefined;
  let cachedKey: string | undefined;

  // Mirrors dev.perfetto.HeapProfile: if the heap graph is incomplete we gate
  // the flamegraph behind a dismissible warning modal. Keyed by dump so it
  // re-arms when the dump changes; the check runs (and the modal is shown) only
  // when this view is rendered, i.e. when the flamegraph tab is active.
  const incompleteSlot = new QuerySlot<{
    isIncomplete: boolean;
    dismissed: boolean;
  }>();

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

      const incomplete = incompleteSlot.use({
        key: {upid: attrs.upid, ts: attrs.ts},
        queryFn: async () => ({
          isIncomplete: await isHeapGraphIncomplete(attrs.trace),
          dismissed: false,
        }),
      }).data;

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
        incomplete !== undefined &&
          incomplete.isIncomplete &&
          !incomplete.dismissed &&
          incompleteFlamegraphModal(attrs.trace, () => {
            incomplete.dismissed = true;
          }),
        m(FlamegraphPanel, {
          trace: attrs.trace,
          metrics,
          state,
          onStateChange: attrs.onStateChange,
        }),
      );
    },
  };
}
