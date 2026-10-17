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

// The Field Flamegraph: the heap dump flamegraph with the field holding every
// object as a frame between its parent and its class
// (SystemServer -> .mPackageManagerService -> PackageManagerService -> ...),
// so that retention can be reasoned about per field rather than per class.
// The trees are built by the android.memory.heap_graph.*field_tree modules.

import m from 'mithril';
import type {Trace} from '../../../public/trace';
import type {time} from '../../../base/time';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../../components/tree_explorer_fetcher';
import {Memo} from '../../../base/memo';
import {TreeExplorerPanel} from '../../../components/tree_explorer_panel';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerState,
  type TreeExplorerOptionalAction,
} from '../../../widgets/tree_explorer';
import {AsyncMemo} from '../../../base/async_memo';
import {
  isHeapGraphIncomplete,
  incompleteFlamegraphModal,
} from '../../dev.perfetto.HeapProfile/incomplete_flamegraph';
import {
  mergedClassNodeIcon,
  mergedClassNodeTooltipRenderer,
} from '../../../components/merged_class_flamegraph_hooks';
import {
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
  MERGED_CLASS_PROPERTY,
  mergedClassColumnSql,
} from '../../dev.perfetto.HeapProfile/merged_class_sql';
import {
  METRIC_DOMINATED_OBJECT_SIZE,
  METRIC_OBJECT_SIZE,
} from './flamegraph_view';

interface FieldFlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
  // Open the flamegraph-objects tab for `pathHashes` (CSV) of a field tree.
  readonly onShowObjects: (pathHashes: string, isDominator: boolean) => void;
}

// field_name and field_type_name are only set on field frames, so they show in
// the tooltip of field frames only. node_kind keeps a field frame from ever
// merging with a class frame.
const UNAGG_PROPS = [
  {name: 'field_name', displayName: 'Field'},
  {name: 'field_type_name', displayName: 'Field type'},
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
  {name: 'node_kind', displayName: 'Frame kind', isVisible: () => false},
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
];

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Concatenated when frames merge (bottom-up, pivot) so that "Show objects"
// lists the objects of every merged frame.
const PATH_HASH_AGG_PROP = {
  name: 'path_hash_stable',
  displayName: 'Path Hash',
  mergeAggregation: 'CONCAT_WITH_COMMA' as const,
  isVisible: () => false,
};

const MERGED_CLASS_NODE_ICON = mergedClassNodeIcon(MERGED_CLASS_PROPERTY);
const MERGED_CLASS_NODE_TOOLTIP = mergedClassNodeTooltipRenderer(
  MERGED_CLASS_PROPERTY,
);

interface MetricSpec {
  readonly name: string;
  readonly unit: string;
  readonly valueColumn: 'self_size' | 'self_count';
  readonly isDominator: boolean;
}

// Same names as the Flamegraph tab so that the two read alike.
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

function buildMetric(
  upid: number,
  ts: time,
  spec: MetricSpec,
  showObjectsAction: TreeExplorerOptionalAction,
): TreeExplorerQueryMetric {
  const [module, tree] = spec.isDominator
    ? ['dominator_field_tree', '_heap_graph_dominator_field_tree']
    : ['field_tree', '_heap_graph_field_tree'];
  const otherCol =
    spec.valueColumn === 'self_size' ? 'self_count' : 'self_size';
  return {
    name: spec.name,
    unit: spec.unit,
    dependencySql: `include perfetto module android.memory.heap_graph.${module};`,
    statement: `
      select
        id,
        parent_id as parentId,
        ifnull(name, 'unknown') as name,
        field_name,
        field_type_name,
        root_type,
        heap_type,
        node_kind,
        ${spec.valueColumn} as value,
        ${otherCol},
        CAST(path_hash_stable AS TEXT) AS path_hash_stable,
        ${mergedClassColumnSql('t.name', upid, ts)}
      from ${tree} t
      where graph_sample_ts = ${ts} and upid = ${upid}
    `,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties:
      spec.valueColumn === 'self_size'
        ? [SELF_COUNT_AGG_PROP, PATH_HASH_AGG_PROP]
        : [PATH_HASH_AGG_PROP],
    optionalNodeActions: [showObjectsAction],
  };
}

function buildMetrics(
  upid: number,
  ts: time,
  onShowObjects: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<TreeExplorerQueryMetric> {
  return METRIC_SPECS.map((spec) =>
    buildMetric(upid, ts, spec, {
      name: 'Show objects',
      icon: 'data_object',
      category: 'DRILL',
      description:
        'List the objects of this frame: for a field, the objects it holds.',
      execute: async ({properties}) => {
        const pathHashes = properties.get('path_hash_stable');
        if (pathHashes === undefined) return;
        onShowObjects(pathHashes, spec.isDominator);
      },
    }),
  );
}

export function FieldFlamegraphView(): m.Component<FieldFlamegraphViewAttrs> {
  // The fetcher is created for the dump it serves and disposed by the memo when
  // the dump changes or when this view is removed.
  const fetcherMemo = new Memo<TreeExplorerFetcher>();
  const incompleteSlot = new AsyncMemo<{
    isIncomplete: boolean;
    dismissed: boolean;
  }>();

  return {
    view({attrs}) {
      const fetcher = fetcherMemo.use({
        key: {upid: attrs.upid, ts: attrs.ts},
        compute: () =>
          new TreeExplorerFetcher(
            attrs.trace,
            buildMetrics(attrs.upid, attrs.ts, attrs.onShowObjects),
          ),
      });

      const incomplete = incompleteSlot.use({
        key: {upid: attrs.upid, ts: attrs.ts},
        compute: async () => ({
          isIncomplete: await isHeapGraphIncomplete(attrs.trace),
          dismissed: false,
        }),
      }).data;

      let state = attrs.state;
      if (state === undefined) {
        state = createDefaultTreeExplorerState(fetcher.metrics);
        attrs.onStateChange(state);
      }

      return [
        incomplete !== undefined &&
          incomplete.isIncomplete &&
          !incomplete.dismissed &&
          incompleteFlamegraphModal(attrs.trace, () => {
            incomplete.dismissed = true;
          }),
        m(TreeExplorerPanel, {
          fetcher,
          state,
          onStateChange: attrs.onStateChange,
          nodeIcon: MERGED_CLASS_NODE_ICON,
          renderNodeTooltip: MERGED_CLASS_NODE_TOOLTIP,
        }),
      ];
    },
    onremove() {
      fetcherMemo.dispose();
    },
  };
}
