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

// Full-height interactive Native Heap Profile (heapprofd) Flamegraph,
// synchronized with the selected process and timeline window [baseTs, selTs].

import m from 'mithril';
import {AsyncMemo} from '../../../../../base/async_memo';
import {Memo} from '../../../../../base/memo';
import {sqliteString} from '../../../../../base/string_utils';
import type {time} from '../../../../../base/time';
import {
  metricsFromTableOrSubquery,
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../../../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../../../../components/tree_explorer_panel';
import type {Trace} from '../../../../../public/trace';
import {STR} from '../../../../../trace_processor/query_result';
import {
  Button,
  ButtonGroup,
  ButtonVariant,
} from '../../../../../widgets/button';
import {DetailsShell} from '../../../../../widgets/details_shell';
import {EmptyState} from '../../../../../widgets/empty_state';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerState,
} from '../../../../../widgets/tree_explorer';

export interface NativeFlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly selTs?: time;
  readonly baseTs?: time;
}

const ALL_HEAPS = '__all__';

async function loadNativeHeaps(
  trace: Trace,
  upid: number,
): Promise<readonly string[]> {
  const res = await trace.engine.query(`
    SELECT DISTINCT ifnull(heap_name, 'libc.malloc') AS heap_name
    FROM heap_profile_allocation
    WHERE upid = ${upid}
    ORDER BY heap_name ASC
  `);
  const heaps: string[] = [];
  for (const it = res.iter({heap_name: STR}); it.valid(); it.next()) {
    heaps.push(it.heap_name);
  }
  return heaps;
}

function buildNativeMetrics(
  upid: number,
  fromTs: time | undefined,
  toTs: time | undefined,
  heapName: string,
): ReadonlyArray<TreeExplorerQueryMetric> {
  const windowWhere =
    (fromTs !== undefined ? ` AND a.ts > ${fromTs}` : '') +
    (toTs !== undefined ? ` AND a.ts <= ${toTs}` : '');
  const heapWhere =
    heapName !== ALL_HEAPS
      ? ` AND ifnull(a.heap_name, 'libc.malloc') = ${sqliteString(heapName)}`
      : '';

  return metricsFromTableOrSubquery({
    tableOrSubquery: `
      (
        WITH alloc_class AS (
          SELECT callsite_id, IIF(SUM(count) > 0, 1, 0) AS positive_alloc
          FROM heap_profile_allocation a
          WHERE a.upid = ${upid}${windowWhere}${heapWhere}
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
            IIF(positive_alloc, a.size, 0) AS size,
            IIF(positive_alloc, a.count, 0) AS count,
            MAX(a.size, 0) AS alloc_size,
            MAX(a.count, 0) AS alloc_count
          FROM heap_profile_allocation a
          JOIN alloc_class USING (callsite_id)
          WHERE a.upid = ${upid}${windowWhere}${heapWhere}
        ))
      )
    `,
    tableMetrics: [
      {
        name: 'Unreleased Size',
        unit: 'B',
        columnName: 'self_size',
      },
      {
        name: 'Unreleased Count',
        unit: '',
        columnName: 'self_count',
      },
      {
        name: 'Total Allocated Size',
        unit: 'B',
        columnName: 'self_alloc_size',
      },
      {
        name: 'Total Allocated Count',
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

export class NativeFlamegraphView implements m.ClassComponent<NativeFlamegraphViewAttrs> {
  private readonly heapsSlot = new AsyncMemo<readonly string[]>();
  private readonly fetcherMemo = new Memo<TreeExplorerFetcher>();
  private selectedHeap = ALL_HEAPS;
  private treeState?: TreeExplorerState;

  onremove() {
    this.heapsSlot.dispose();
    this.fetcherMemo.dispose();
  }

  view({attrs}: m.Vnode<NativeFlamegraphViewAttrs>): m.Children {
    const {trace, upid, selTs, baseTs} = attrs;
    const heaps = this.heapsSlot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadNativeHeaps(trace, upid),
    }).data;

    if (heaps === undefined) {
      return m(
        '.pf-memtree-empty',
        m(EmptyState, {
          icon: 'hourglass',
          title: 'Loading native heap profile…',
        }),
      );
    }

    if (heaps.length === 0) {
      return m(
        '.pf-memtree-empty',
        m(
          EmptyState,
          {
            icon: 'local_fire_department',
            title: 'No native heap profile (heapprofd) data for this process',
          },
          'Capture a heapprofd profile to inspect native allocation flamegraphs.',
        ),
      );
    }

    const activeHeap =
      this.selectedHeap === ALL_HEAPS || heaps.includes(this.selectedHeap)
        ? this.selectedHeap
        : ALL_HEAPS;

    const fetcher = this.fetcherMemo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        from: baseTs !== undefined ? baseTs.toString() : null,
        to: selTs !== undefined ? selTs.toString() : null,
        heap: activeHeap,
      },
      compute: () =>
        new TreeExplorerFetcher(
          trace,
          buildNativeMetrics(upid, baseTs, selTs, activeHeap),
        ),
    });

    if (this.treeState === undefined) {
      this.treeState = createDefaultTreeExplorerState(fetcher.metrics);
    }

    const secOf = (ts: time) =>
      `${(Number(ts - trace.traceInfo.start) / 1e9).toFixed(1)}s`;
    const windowLabel =
      baseTs !== undefined && selTs !== undefined
        ? `Δ window ${secOf(baseTs)} → ${secOf(selTs)}`
        : selTs !== undefined
          ? `Cumulative up to t=${secOf(selTs)}`
          : 'Full trace (cumulative unreleased)';

    return m(
      DetailsShell,
      {
        title: 'Native Heap Flamegraph',
        description: windowLabel,
        fillHeight: true,
        buttons:
          heaps.length > 1 &&
          m(ButtonGroup, [
            m(Button, {
              label: 'All heaps',
              compact: true,
              variant: ButtonVariant.Outlined,
              active: activeHeap === ALL_HEAPS,
              onclick: () => {
                this.selectedHeap = ALL_HEAPS;
              },
            }),
            heaps.map((h) =>
              m(Button, {
                key: h,
                label: h,
                compact: true,
                variant: ButtonVariant.Outlined,
                active: activeHeap === h,
                onclick: () => {
                  this.selectedHeap = h;
                },
              }),
            ),
          ]),
      },
      m(TreeExplorerPanel, {
        fetcher,
        state: this.treeState,
        onStateChange: (next) => {
          this.treeState = next;
        },
      }),
    );
  }
}
