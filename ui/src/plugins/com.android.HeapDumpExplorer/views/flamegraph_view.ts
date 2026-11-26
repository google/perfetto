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
  convertTrace,
  type PprofProfileType,
} from '../../../base/trace_converter';

import {Button} from '../../../widgets/button';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {showModal} from '../../../widgets/modal';
import {Inline, Stack, StackAuto, StackFixed} from '../../../widgets/stack';
import {
  mergedClassNodeIcon,
  mergedClassNodeTooltipRenderer,
} from '../../../components/merged_class_flamegraph_hooks';
import {
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
  MERGED_CLASS_PROPERTY,
  mergedClassNamesSql,
} from '../../dev.perfetto.HeapProfile/merged_class_sql';
import {NUM} from '../../../trace_processor/query_result';
import type {HeapDump} from '../types';

// Referenced by session.openFlamegraphPivotedAt.
export const METRIC_OBJECT_SIZE = 'Object Size';
export const METRIC_DOMINATED_OBJECT_SIZE = 'Dominated Object Size';

interface FlamegraphViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  // Every heap dump of the trace: this one can be compared with the others.
  readonly dumps: ReadonlyArray<HeapDump>;
  // The dump the flamegraph is compared with, if any.
  readonly compareWith: HeapDump | undefined;
  readonly onCompareWithChange: (dump: HeapDump | undefined) => void;
  readonly dumpLabel: (dump: HeapDump) => string;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
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
  MERGED_CLASS_FLAMEGRAPH_PROPERTY,
  {
    name: 'path_hash_stable',
    displayName: 'Path Hash',
    isVisible: () => false,
  },
];

// Flag potentially merged classes (see merged_class_sql.ts).
const MERGED_CLASS_NODE_ICON = mergedClassNodeIcon(MERGED_CLASS_PROPERTY);
const MERGED_CLASS_NODE_TOOLTIP = mergedClassNodeTooltipRenderer(
  MERGED_CLASS_PROPERTY,
);

const SELF_COUNT_AGG_PROP = {
  name: 'self_count',
  displayName: 'Self Count',
  mergeAggregation: 'SUM' as const,
};

// Several dumps can't be grouped by path_hash_stable: the hash is computed per
// dump, so it would keep their nodes apart. It is concatenated instead, as
// dev.perfetto.HeapProfile does, so that the objects of a node can still be
// listed.
const MULTI_DUMP_UNAGG_PROPS = UNAGG_PROPS.filter(
  (p) => p.name !== 'path_hash_stable',
);
const PATH_HASH_AGG_PROP = {
  name: 'path_hash_stable',
  displayName: 'Path Hash',
  mergeAggregation: 'CONCAT_WITH_COMMA' as const,
  isVisible: () => false,
};

type DumpRef = Pick<HeapDump, 'upid' | 'ts'>;

function inDumpsSql(dumps: ReadonlyArray<DumpRef>): string {
  return dumps
    .map((d) => `(upid = ${d.upid} and graph_sample_ts = ${d.ts})`)
    .join(' or ');
}

// '1' if the class named by `t.name` is potentially merged in any of `dumps`.
function potentiallyMergedColumnSql(dumps: ReadonlyArray<DumpRef>): string {
  const names = dumps
    .map((d) => `select name from (${mergedClassNamesSql(d.upid, d.ts)})`)
    .join(' union ');
  return `CAST(t.name IN (${names}) AS TEXT) AS ${MERGED_CLASS_PROPERTY}`;
}

// Build a JAVA_HEAP_GRAPH metric for the BFS or dominator class tree of
// `dump`, projecting `valueColumn` as `value` and the other column for
// tooltips. The values of `baselineDump`, if any, are the baseline.
function buildMetric(
  dump: DumpRef,
  baselineDump: DumpRef | undefined,
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
  const compared = baselineDump !== undefined;
  const allDumps = compared ? [dump, baselineDump] : [dump];
  // `column` on the rows of `of`, and 0 on the rows of the other dump.
  const valueOf = (column: string, of: DumpRef) =>
    compared ? `iif(${inDumpsSql([of])}, ${column}, 0)` : column;
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
        ${valueOf(valueColumn, dump)} as value,
        ${valueOf(otherCol, dump)} as ${otherCol},
        iif(${inDumpsSql([dump])}, CAST(path_hash_stable AS TEXT), NULL)
          AS path_hash_stable,
        ${compared ? `${valueOf(valueColumn, baselineDump)} as baseline,` : ''}
        ${potentiallyMergedColumnSql(allDumps)}
      from ${tree} t
      where ${inDumpsSql(allDumps)}
    `,
    baselineColumn: compared ? 'baseline' : undefined,
    unaggregatableProperties: compared ? MULTI_DUMP_UNAGG_PROPS : UNAGG_PROPS,
    aggregatableProperties: [
      ...(valueColumn === 'self_size' ? [SELF_COUNT_AGG_PROP] : []),
      ...(compared ? [PATH_HASH_AGG_PROP] : []),
    ],
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

export function buildHeapGraphMetrics(
  dump: DumpRef,
  baselineDump: DumpRef | undefined,
  onShowObjects: (pathHashes: string, isDominator: boolean) => void,
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
      onShowObjects(pathHashes, isDominator);
    },
  });
  return METRIC_SPECS.map((s) =>
    buildMetric(
      dump,
      baselineDump,
      s.name,
      s.unit,
      s.valueColumn,
      s.isDominator,
      showObjectsAction(s.isDominator),
    ),
  );
}

function renderComparePicker(attrs: FlamegraphViewAttrs): m.Children {
  const others = attrs.dumps.filter(
    (d) => !(d.upid === attrs.upid && d.ts === attrs.ts),
  );
  if (others.length === 0) return null;
  const {compareWith, dumpLabel} = attrs;
  return m(
    StackFixed,
    {className: 'pf-hde-flamegraph-dumps'},
    m(Inline, {spacing: 'small'}, [
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label:
              compareWith === undefined
                ? 'Compare with'
                : `Compare with ${dumpLabel(compareWith)}`,
            icon: 'compare_arrows',
            rightIcon: 'arrow_drop_down',
            compact: true,
          }),
        },
        [
          m(MenuItem, {
            label: 'No comparison',
            active: compareWith === undefined,
            onclick: () => attrs.onCompareWithChange(undefined),
          }),
          ...others.map((d) =>
            m(MenuItem, {
              label: dumpLabel(d),
              active: d === compareWith,
              onclick: () => attrs.onCompareWithChange(d),
            }),
          ),
        ],
      ),
      compareWith !== undefined &&
        m(
          'span.pf-hde-flamegraph-dumps__hint',
          'Red: more than the compared dump, blue: less.',
        ),
    ]),
  );
}

export function FlamegraphView(): m.Component<FlamegraphViewAttrs> {
  // The fetcher is created for the dumps it serves and disposed by the memo
  // when they change or when this view is removed.
  const fetcherMemo = new Memo<TreeExplorerFetcher>();

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
      const dump: DumpRef = {upid: attrs.upid, ts: attrs.ts};
      const baselineDump = attrs.compareWith;
      const ref = (d: DumpRef | undefined) =>
        d === undefined ? null : {upid: d.upid, ts: d.ts};
      const fetcher = fetcherMemo.use({
        key: {dump: ref(dump), baselineDump: ref(baselineDump)},
        compute: () =>
          new TreeExplorerFetcher(
            attrs.trace,
            buildHeapGraphMetrics(dump, baselineDump, attrs.onShowObjects),
          ),
      });
      const metrics = fetcher.metrics;

      const incomplete = incompleteSlot.use({
        key: {upid: attrs.upid, ts: attrs.ts},
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

      return [
        incomplete !== undefined &&
          incomplete.isIncomplete &&
          !incomplete.dismissed &&
          incompleteFlamegraphModal(attrs.trace, () => {
            incomplete.dismissed = true;
          }),
        m(Stack, {fillHeight: true, spacing: 'none'}, [
          renderComparePicker(attrs),
          m(
            StackAuto,
            m(TreeExplorerPanel, {
              fetcher,
              state,
              onStateChange: attrs.onStateChange,
              nodeIcon: MERGED_CLASS_NODE_ICON,
              renderNodeTooltip: MERGED_CLASS_NODE_TOOLTIP,
              // A pprof holds one snapshot, so it can't show a diff.
              extraDownloadItems:
                baselineDump !== undefined
                  ? []
                  : [
                      {
                        label: 'Pprof profile (.pb)',
                        icon: 'file_download',
                        description:
                          'Whole snapshot, converted from the trace: ' +
                          'filters, the selected measure and the view ' +
                          'direction are not applied.',
                        title:
                          'Download the full profile as pprof, for use ' +
                          'with pprof tools',
                        onDownload: () =>
                          downloadPprof(attrs.trace, attrs.upid, attrs.ts),
                      },
                    ],
            }),
          ),
        ]),
      ];
    },
    onremove() {
      fetcherMemo.dispose();
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
