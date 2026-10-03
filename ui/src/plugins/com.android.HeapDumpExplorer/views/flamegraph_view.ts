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
import {download} from '../../../base/download_utils';
import type {Trace} from '../../../public/trace';
import type {time} from '../../../base/time';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../../components/tree_explorer_fetcher';
import {Memo} from '../../../base/memo';
import {
  type TreeExplorerBaseline,
  TreeExplorerPanel,
} from '../../../components/tree_explorer_panel';
import {
  createDefaultTreeExplorerState,
  getTreeExplorerComparison,
  type TreeExplorerState,
  type TreeExplorerOptionalAction,
} from '../../../widgets/tree_explorer';
import {AsyncMemo} from '../../../base/async_memo';
import {
  isHeapGraphIncomplete,
  incompleteFlamegraphModal,
} from '../../dev.perfetto.HeapProfile/incomplete_flamegraph';
import {
  convertTrace,
  type PprofProfileType,
} from '../../../base/trace_converter';

import {showModal} from '../../../widgets/modal';
import {NUM} from '../../../trace_processor/query_result';

// Referenced by session.openFlamegraphPivotedAt.
export const METRIC_OBJECT_SIZE = 'Object Size';
export const METRIC_DOMINATED_OBJECT_SIZE = 'Dominated Object Size';

// A heap dump, as identified in the trace, and a short name for it.
interface DumpRef {
  readonly upid: number;
  readonly ts: time;
  readonly label: string;
}

// The objects of a class node of a dump's tree.
export interface FlamegraphObjectsSelection {
  // The node's path_hash_stable (CSV).
  readonly pathHashes: string;
  readonly isDominator: boolean;
  readonly upid: number;
  readonly ts: time;
}

interface FlamegraphViewAttrs {
  readonly trace: Trace;
  readonly dump: DumpRef;
  // The dump to compare `dump` against, if any.
  readonly baseline?: DumpRef;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
  // Open the flamegraph-objects tab for a node of either dump's tree.
  readonly onShowObjects: (selection: FlamegraphObjectsSelection) => void;
}

// path_hash_stable is exposed unaggregatable (and CAST to TEXT in SQL,
// since the stdlib emits it as INT64 and the flamegraph reads
// unaggregatable columns as STR_NULL) so it lands in `matchingColumns`
// — that's what lets a PIVOT filter target a specific node by its hash.
// Hidden from the tooltip via `isVisible: false`. The hash is seeded with
// the dump's upid and timestamp, so diffs must not pair nodes by it.
const UNAGG_PROPS = [
  {name: 'root_type', displayName: 'Root Type'},
  {name: 'heap_type', displayName: 'Heap Type'},
  {
    name: 'path_hash_stable',
    displayName: 'Path Hash',
    isVisible: () => false,
    profileSpecific: true,
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
  showObjectsAction: TreeExplorerOptionalAction,
): TreeExplorerQueryMetric {
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
  onShowObjects: (selection: FlamegraphObjectsSelection) => void,
): ReadonlyArray<TreeExplorerQueryMetric> {
  const showObjectsAction = (
    isDominator: boolean,
  ): TreeExplorerOptionalAction => ({
    name: 'Show objects from this class',
    icon: 'data_object',
    category: 'DRILL',
    description: 'List the individual objects of this class.',
    execute: async ({properties}) => {
      const pathHashes = properties.get('path_hash_stable');
      if (pathHashes === undefined) return;
      onShowObjects({pathHashes, isDominator, upid, ts});
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
  // The fetchers are created for the dumps they serve and disposed by the
  // memos when the dumps change or when this view is removed.
  const fetcherMemo = new Memo<TreeExplorerFetcher>();
  const baselineFetcherMemo = new Memo<TreeExplorerFetcher>();
  const createFetcher = (attrs: FlamegraphViewAttrs, dump: DumpRef) =>
    new TreeExplorerFetcher(
      attrs.trace,
      buildHeapGraphMetrics(dump.upid, dump.ts, attrs.onShowObjects),
    );

  // Mirrors dev.perfetto.HeapProfile: if the heap graph is incomplete we gate
  // the flamegraph behind a dismissible warning modal. Keyed by dump so it
  // re-arms when the dump changes; the check runs (and the modal is shown) only
  // when this view is rendered, i.e. when the flamegraph tab is active.
  const incompleteSlot = new AsyncMemo<{
    isIncomplete: boolean;
    dismissed: boolean;
  }>();

  return {
    view({attrs}) {
      const {dump, baseline} = attrs;
      const fetcher = fetcherMemo.use({
        key: {upid: dump.upid, ts: dump.ts},
        compute: () => createFetcher(attrs, dump),
      });
      const metrics = fetcher.metrics;
      let panelBaseline: TreeExplorerBaseline | undefined;
      if (baseline === undefined) {
        baselineFetcherMemo.invalidate();
      } else {
        panelBaseline = {
          fetcher: baselineFetcherMemo.use({
            key: {upid: baseline.upid, ts: baseline.ts},
            compute: () => createFetcher(attrs, baseline),
          }),
          baselineLabel: baseline.label,
          currentLabel: dump.label,
        };
      }

      const incomplete = incompleteSlot.use({
        key: {upid: dump.upid, ts: dump.ts},
        compute: async () => ({
          isIncomplete: await isHeapGraphIncomplete(attrs.trace),
          dismissed: false,
        }),
      }).data;

      // First render or after a dump-change reset: create a default
      // state so the panel renders meaningfully on the same frame.

      let state = attrs.state;
      if (state === undefined) {
        state = createDefaultTreeExplorerState(metrics);
        attrs.onStateChange(state);
      }

      // A pprof profile holds a single dump: the one shown, or in a diff the
      // current one.
      const pprofDump =
        baseline !== undefined &&
        getTreeExplorerComparison(state).show === 'BASELINE'
          ? baseline
          : dump;

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
          baseline: panelBaseline,
          extraDownloadItems: [
            {
              label: 'Pprof profile (.pb)',
              icon: 'file_download',
              description:
                'Whole snapshot' +
                (baseline === undefined ? '' : ` of ${pprofDump.label}`) +
                ', converted from the trace: filters, the selected measure ' +
                'and the view direction are not applied.',
              title:
                'Download the full profile as pprof, for use with pprof tools',
              onDownload: () =>
                downloadPprof(attrs.trace, pprofDump.upid, pprofDump.ts),
            },
          ],
        }),
      ];
    },
    onremove() {
      fetcherMemo.dispose();
      baselineFetcherMemo.dispose();
    },
  };
}

const HEAP_PPROFILE_TYPE: PprofProfileType = 'java-heap';

async function downloadPprof(trace: Trace, upid: number, ts: time) {
  const pid = await trace.engine.query(
    `select pid from process where upid = ${upid}`,
  );
  if (!trace.traceInfo.downloadable) {
    showModal({
      title: 'Download not supported',
      content: m('div', 'This trace file does not support downloads'),
    });
    return;
  }
  const blob = await trace.getTraceFile();
  const result = await convertTrace(blob, {
    format: 'pprof',
    profileType: HEAP_PPROFILE_TYPE,
    pid: pid.firstRow({pid: NUM}).pid,
    ts,
    onStatus: (s) => trace.omnibox.showStatusMessage(s),
  });
  if (!result.ok) {
    showModal({
      title: 'Pprof conversion failed',
      content: m('div', result.error.message),
    });
    return;
  }
  download({content: result.result.buffer, fileName: result.result.name});
}
