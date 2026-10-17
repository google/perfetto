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
import type {time} from '../../../base/time';
import type {Trace} from '../../../public/trace';
import type {TreeExplorerQueryMetric} from '../../../components/tree_explorer_fetcher';
import type {
  TreeExplorerOptionalAction,
  TreeExplorerState,
} from '../../../widgets/tree_explorer';
import {
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
  mergedClassColumnSql,
} from '../../dev.perfetto.HeapProfile/merged_class_sql';
import {HeapGraphTreeView} from './heap_graph_tree_view';

interface CollectionsFlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
  // Open the collections tab for `pathHashes` (CSV). `collectionCls` is set
  // for collection frames.
  readonly onShowCollections: (
    pathHashes: string,
    label: string,
    collectionCls?: string,
  ) => void;
}

const METRICS = [
  {name: 'Retained Size', unit: 'B', column: 'self_size'},
  {name: 'Elements', unit: '', column: 'self_element_count'},
] as const;

// path_hash_stable is unaggregatable (and TEXT) so that merged frames report
// all their hashes to the Show collections action. collection_class is set on
// collection frames so drilling down on one opens its instances.
const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
  {
    name: 'collection_class',
    displayName: 'Collection Class',
    isVisible: () => false,
  },
  {
    name: 'path_hash_stable',
    displayName: 'Path Hash',
    isVisible: () => false,
  },
];

const INSTANCES_AGG_PROP = {
  name: 'self_count',
  displayName: 'Instances',
  mergeAggregation: 'SUM' as const,
};

const ELEMENTS_AGG_PROP = {
  name: 'self_element_count',
  displayName: 'Elements',
  mergeAggregation: 'SUM' as const,
};

function buildMetrics(
  upid: number,
  ts: time,
  onShowCollections: CollectionsFlamegraphViewAttrs['onShowCollections'],
): TreeExplorerQueryMetric[] {
  const showCollections: TreeExplorerOptionalAction = {
    name: 'Show collections',
    icon: 'data_object',
    category: 'DRILL',
    description: 'List the collections for this frame.',
    execute: ({properties, node}) => {
      const pathHashes = properties.get('path_hash_stable');
      if (pathHashes === undefined) return;
      const collectionCls = properties.get('collection_class');
      onShowCollections(pathHashes, node?.name ?? '', collectionCls);
    },
  };
  return METRICS.map((metric) => ({
    name: metric.name,
    unit: metric.unit,
    dependencySql:
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_tree;',
    statement: `
      SELECT
        id,
        parent_id AS parentId,
        t.name,
        collection_class,
        root_type,
        heap_type,
        ${metric.column} AS value,
        self_count,
        self_element_count,
        CAST(path_hash_stable AS TEXT) AS path_hash_stable,
        ${mergedClassColumnSql('coalesce(t.collection_class, t.name)', upid, ts)}
      FROM _heap_graph_collection_tree t
      WHERE upid = ${upid} AND graph_sample_ts = ${ts}
    `,
    unaggregatableProperties: UNAGG_PROPS,
    aggregatableProperties:
      metric.column === 'self_size'
        ? [INSTANCES_AGG_PROP, ELEMENTS_AGG_PROP]
        : [INSTANCES_AGG_PROP],
    optionalNodeActions: [showCollections],
  }));
}

// Flamegraph of the paths from the roots to the collections. Collections are
// frames, and collections owned by another collection are frames below it.
export function CollectionsFlamegraphView(): m.Component<CollectionsFlamegraphViewAttrs> {
  return {
    view({attrs}) {
      return m(HeapGraphTreeView, {
        trace: attrs.trace,
        upid: attrs.upid,
        ts: attrs.ts,
        state: attrs.state,
        onStateChange: attrs.onStateChange,
        buildMetrics: (upid, ts) =>
          buildMetrics(upid, ts, attrs.onShowCollections),
      });
    },
  };
}
