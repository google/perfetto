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

import './native.scss';
import m from 'mithril';
import {
  metricsFromTableOrSubquery,
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../../../components/tree_explorer_panel';
import type {Trace} from '../../../../public/trace';
import {
  addFilter,
  createDefaultTreeExplorerState,
  updateTreeExplorerState,
  type TreeExplorerState,
} from '../../../../widgets/tree_explorer';
import {
  heapWhereClause,
  type NativeProfileMeta,
  type NativeWindow,
} from './native_queries';
import {renderNativeToolbar} from './native_toolbar';

function buildNativeFlamegraphMetrics(
  upid: number,
  win: NativeWindow,
): ReadonlyArray<TreeExplorerQueryMetric> {
  const where = heapWhereClause(upid, win);
  return metricsFromTableOrSubquery({
    tableOrSubquery: `
      (
        WITH alloc_class AS (
          SELECT callsite_id, iif(SUM(count) > 0 OR SUM(size) > 0, 1, 0) AS positive_alloc
          FROM heap_profile_allocation a
          WHERE ${where}
          GROUP BY callsite_id
        )
        SELECT
          id,
          parent_id AS parentId,
          name,
          mapping_name,
          source_file || ':' || line_number AS source_location,
          self_size,
          self_count,
          self_alloc_size,
          self_alloc_count
        FROM _android_heap_profile_callstacks_for_allocations!((
          SELECT
            a.callsite_id,
            iif(c.positive_alloc, a.size, 0) AS size,
            iif(c.positive_alloc, a.count, 0) AS count,
            MAX(a.size, 0) AS alloc_size,
            MAX(a.count, 0) AS alloc_count
          FROM heap_profile_allocation a
          JOIN alloc_class c USING (callsite_id)
          WHERE ${where}
        ))
      )
    `,
    tableMetrics: [
      {
        name: 'Unreleased Malloc Size',
        unit: 'B',
        columnName: 'self_size',
      },
      {
        name: 'Unreleased Malloc Count',
        unit: '',
        columnName: 'self_count',
      },
      {
        name: 'Total Malloc Size (Churn)',
        unit: 'B',
        columnName: 'self_alloc_size',
      },
      {
        name: 'Total Malloc Count (Churn)',
        unit: '',
        columnName: 'self_alloc_count',
      },
    ],
    dependencySql:
      'include perfetto module android.memory.heap_profile.callstacks',
    unaggregatableProperties: [{name: 'mapping_name', displayName: 'Mapping'}],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
    nameColumnLabel: 'Symbol',
  });
}

export interface NativeFlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly meta: NativeProfileMeta;
  readonly window: NativeWindow;
  readonly onWindowChange: (next: NativeWindow) => void;
  readonly initialFilter?: string;
  readonly onConsumedFilter?: () => void;
}

export class NativeFlamegraphView implements m.ClassComponent<NativeFlamegraphViewAttrs> {
  private fetcher?: TreeExplorerFetcher;
  private fetcherKey?: string;
  private state?: TreeExplorerState;
  private appliedInitialFilter?: string;

  onremove() {
    this.fetcher?.[Symbol.dispose]();
    this.fetcher = undefined;
  }

  view({attrs}: m.CVnode<NativeFlamegraphViewAttrs>): m.Children {
    const {
      trace,
      upid,
      meta,
      window: win,
      onWindowChange,
      initialFilter,
      onConsumedFilter,
    } = attrs;

    const key = [
      trace.traceInfo.uuid,
      upid,
      win.heapName,
      win.fromTs?.toString() ?? 'start',
      win.toTs?.toString() ?? 'end',
    ].join('|');

    if (this.fetcher === undefined || this.fetcherKey !== key) {
      this.fetcher?.[Symbol.dispose]();
      const metrics = buildNativeFlamegraphMetrics(upid, win);
      this.fetcher = new TreeExplorerFetcher(trace, metrics);
      this.fetcherKey = key;
      this.state =
        this.state === undefined
          ? createDefaultTreeExplorerState(metrics)
          : updateTreeExplorerState(this.state, metrics);
    }

    if (
      initialFilter !== undefined &&
      initialFilter !== '' &&
      initialFilter !== this.appliedInitialFilter &&
      this.state !== undefined
    ) {
      this.appliedInitialFilter = initialFilter;
      const alreadyHas = this.state.filters.some(
        (f) => f.kind === 'SHOW_STACK' && f.filter === initialFilter,
      );
      if (!alreadyHas) {
        this.state = addFilter(this.state, {
          kind: 'SHOW_STACK',
          filter: initialFilter,
        });
      }
      onConsumedFilter?.();
    }

    return m('.pf-memscope-native-fg', [
      renderNativeToolbar({
        trace,
        meta,
        window: win,
        onChange: onWindowChange,
      }),
      m(
        '.pf-memscope-native-fg__body',
        m(TreeExplorerPanel, {
          fetcher: this.fetcher,
          state: this.state,
          onStateChange: (next) => {
            this.state = next;
          },
        }),
      ),
    ]);
  }
}
