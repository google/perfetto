// Copyright (C) 2024 The Android Open Source Project
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

import {download} from '../../base/download_utils';
import {extensions} from '../../components/extensions';
import type {time} from '../../base/time';
import {
  type TreeExplorerQueryMetric,
  metricsFromTableOrSubquery,
} from '../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import {FlamegraphProfile} from '../../components/flamegraph_profile';
import {type PprofProfileType, convertTrace} from '../../base/trace_converter';

import {Timestamp} from '../../components/widgets/timestamp';
import type {
  TrackEventDetailsPanel,
  TrackEventDetailsPanelSerializeArgs,
} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import type {ExportDownloadItem} from '../../widgets/export_button';
import {DetailsShell} from '../../widgets/details_shell';
import {showModal} from '../../widgets/modal';
import {incompleteFlamegraphModal} from './incomplete_flamegraph';
import {
  createDefaultTreeExplorerState,
  updateTreeExplorerState,
  type TreeExplorerState,
  TREE_EXPLORER_STATE_SCHEMA,
  type TreeExplorerOptionalAction,
} from '../../widgets/tree_explorer';
import type {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {Stack} from '../../widgets/stack';
import {Anchor} from '../../widgets/anchor';
import {Icon} from '../../widgets/icon';
import {Popup, PopupPosition} from '../../widgets/popup';
import {type ProfileDescriptor, ProfileType} from './common';
import {
  buildOomeCallstackMetrics,
  loadOomeDetails,
  renderOomeDetails,
  type OomeDetails,
} from './oome_callstack_common';

const DOCS_NATIVE_HEAP_PROFILER =
  'https://perfetto.dev/docs/data-sources/native-heap-profiler';
const DOCS_JAVA_HEAP_PROFILER =
  'https://perfetto.dev/docs/data-sources/java-heap-profiler';

// Short "what is this / how do I use it" help shown by the header help icon,
// with a link to the relevant data-source documentation.
function profileHelp(descriptor: ProfileDescriptor): m.Children {
  let what: string;
  let how: string;
  let docs: string;
  switch (descriptor.type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
      what =
        'Callstack-sampled native (malloc/free) allocations recorded by ' +
        'heapprofd over this interval.';
      how =
        'Read the flamegraph to attribute unreleased or heavily-allocated ' +
        'memory to call paths, and switch the metric (retained vs total, ' +
        'size vs count) with the dropdown.';
      docs = DOCS_NATIVE_HEAP_PROFILER;
      break;
    case ProfileType.JAVA_HEAP_SAMPLES:
      what =
        'Callstack-sampled Java/Kotlin allocations recorded by ART over ' +
        'this interval.';
      how =
        'Read the flamegraph to see which call paths allocate the most on ' +
        'the managed heap.';
      docs = DOCS_NATIVE_HEAP_PROFILER + '#art-allocation-profiling';
      break;
    case ProfileType.GENERIC_HEAP_PROFILE:
      what =
        'Callstack-sampled allocations from a custom heapprofd-compatible ' +
        'allocator over this interval.';
      how = 'Read the flamegraph to attribute allocations to call paths.';
      docs = DOCS_NATIVE_HEAP_PROFILER;
      break;
    case ProfileType.JAVA_HEAP_GRAPH:
      what =
        'A full ART heap dump: the retention graph of live Java/Kotlin ' +
        'objects at this point in time.';
      how =
        'Read the flamegraph to see what retains memory; the "Dominated" ' +
        'metrics show memory kept alive exclusively by each node.';
      docs = DOCS_JAVA_HEAP_PROFILER;
      break;
    case ProfileType.OOME_CALLSTACK:
      what = 'The allocation callstack that triggered an OutOfMemoryError.';
      how =
        'Read the flamegraph to see the path that pushed the heap over its ' +
        'limit.';
      docs = DOCS_JAVA_HEAP_PROFILER;
      break;
  }
  return m('.pf-heap-profile-help', [
    m('div', what),
    m('div', how),
    m(
      Anchor,
      {href: docs, target: '_blank', icon: 'open_in_new'},
      'Documentation',
    ),
  ]);
}

// Header title with a help affordance. Hovering the icon shows the help as a
// transient preview (it disappears as soon as the pointer leaves the icon, so
// you cannot reach the docs link). Clicking the icon pins the popup open, so it
// persists and its link becomes clickable, until dismissed by clicking away or
// clicking the icon again.
function HeapProfileTitleHelp(): m.Component<{
  label: string;
  help: m.Children;
}> {
  let pinned = false;
  let hovering = false;
  return {
    view: ({attrs}) =>
      m(
        'span.pf-heap-profile-title-help',
        attrs.label,
        m(
          Popup,
          {
            isOpen: pinned || hovering,
            onChange: (shouldOpen) => {
              // The popup itself only ever asks to close (outside click /
              // escape); honour it by clearing both states.
              if (!shouldOpen) {
                pinned = false;
                hovering = false;
              }
            },
            position: PopupPosition.Bottom,
            trigger: m(Icon, {
              className: 'pf-heap-profile-title-help__icon',
              icon: 'help_outline',
              onmouseenter: () => {
                hovering = true;
              },
              onmouseleave: () => {
                hovering = false;
              },
              onclick: () => {
                pinned = !pinned;
              },
            }),
          },
          attrs.help,
        ),
      ),
  };
}

interface Props {
  ts: time;
  type: ProfileType;
}

export class HeapProfileFlamegraphDetailsPanel implements TrackEventDetailsPanel {
  private readonly props: Props;
  private flamegraphModalDismissed = false;
  private oomeDetails?: OomeDetails;

  // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
  // We moved serialization from being attached to selections to instead being
  // attached to the plugin that loaded the panel.
  readonly serialization: TrackEventDetailsPanelSerializeArgs<
    TreeExplorerState | undefined
  > = {
    schema: TREE_EXPLORER_STATE_SCHEMA.optional(),
    state: undefined,
  };

  readonly metrics: ReadonlyArray<TreeExplorerQueryMetric>;

  constructor(
    private readonly trace: Trace,
    private readonly heapGraphIncomplete: boolean,
    private readonly upid: number,
    private readonly profileDescriptor: ProfileDescriptor,
    private readonly ts: time,
    private readonly tsEnd: time,
    private state: TreeExplorerState | undefined,
    private readonly onStateChange: (state: TreeExplorerState) => void,
    // True when `ts`/`tsEnd` come from an area selection rather than from a
    // single snapshot. traceconv can only convert a snapshot it can name by
    // timestamp, so the pprof export is not offered in that case.
    private readonly isAreaSelection: boolean,
    onNodeSelected?: (args: {
      pathHashes: string;
      isDominator: boolean;
      upid: number;
      ts: time;
    }) => void,
  ) {
    this.props = {ts, type: profileDescriptor.type};
    this.metrics = flamegraphMetrics(
      this.trace,
      this.profileDescriptor,
      this.ts,
      this.tsEnd,
      this.upid,
      onNodeSelected
        ? (pathHashes, isDominator) =>
            onNodeSelected({pathHashes, isDominator, upid, ts})
        : undefined,
    );
    if (this.state === undefined) {
      this.state = createDefaultTreeExplorerState(this.metrics);
      onStateChange(this.state);
    }
  }

  async load() {
    if (this.props.type === ProfileType.OOME_CALLSTACK) {
      this.oomeDetails = await loadOomeDetails(this.trace.engine, this.ts);
      m.redraw();
    }

    // If the state in the serialization is not undefined, we should read from
    // it.
    // TODO(lalitm): remove this in 26Q2 - see comment on `serialization`.
    if (this.serialization.state !== undefined) {
      this.state = updateTreeExplorerState(
        this.serialization.state,
        this.metrics,
      );
      this.onStateChange(this.state);
      this.serialization.state = undefined;
    }
  }

  render() {
    const {type, ts} = this.props;
    return m(
      FlamegraphProfile,
      this.maybeShowModal(this.trace, type, this.heapGraphIncomplete),
      m(
        DetailsShell,
        {
          fillHeight: true,
          title: m(
            Stack,
            {orientation: 'vertical'},
            m(HeapProfileTitleHelp, {
              label: this.profileDescriptor.label,
              help: profileHelp(this.profileDescriptor),
            }),
            renderOomeDetails(this.oomeDetails),
          ),
          buttons: m(
            'span',
            `Snapshot time: `,
            m(Timestamp, {trace: this.trace, ts}),
          ),
        },
        m(TreeExplorerPanel, {
          trace: this.trace,
          metrics: this.metrics,
          state: this.state,
          onStateChange: (state) => {
            this.state = state;
            this.onStateChange(state);
          },
          extraDownloadItems: this.pprofDownloadItems(type, ts),
        }),
      ),
    );
  }

  // The pprof is produced by traceconv from the raw trace, so it always
  // covers the whole snapshot: nothing the tree explorer does to the
  // displayed tree can be reflected in it.
  private pprofDownloadItems(
    type: ProfileType,
    ts: time,
  ): ReadonlyArray<ExportDownloadItem> {
    const profileType = pprofProfileType(type);
    if (profileType === undefined || this.isAreaSelection) {
      return [];
    }
    return [
      {
        label: 'Pprof profile (.pb)',
        icon: 'file_download',
        description:
          'Whole snapshot, converted from the trace: filters, the selected ' +
          'measure and the view direction are not applied.',
        title: 'Download the full profile as pprof, for use with pprof tools',
        onDownload: () => downloadPprof(this.trace, this.upid, ts, profileType),
      },
    ];
  }

  private maybeShowModal(
    trace: Trace,
    type: ProfileType,
    heapGraphIncomplete: boolean,
  ) {
    if (type !== ProfileType.JAVA_HEAP_GRAPH || !heapGraphIncomplete) {
      return undefined;
    }
    if (this.flamegraphModalDismissed) {
      return undefined;
    }
    return incompleteFlamegraphModal(trace, () => {
      this.flamegraphModalDismissed = true;
    });
  }
}

function flamegraphMetrics(
  trace: Trace,
  descriptor: ProfileDescriptor,
  ts: time,
  tsEnd: time,
  upid: number,
  onNodeSelected?: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<TreeExplorerQueryMetric> {
  switch (descriptor.type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
      return flamegraphMetricsForHeapProfile(
        ts,
        tsEnd,
        upid,
        descriptor.heapName!,
        [
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
            name: 'Total Malloc Size',
            unit: 'B',
            columnName: 'self_alloc_size',
          },
          {
            name: 'Total Malloc Count',
            unit: '',
            columnName: 'self_alloc_count',
          },
        ],
      );
    case ProfileType.GENERIC_HEAP_PROFILE:
      return flamegraphMetricsForHeapProfile(
        ts,
        tsEnd,
        upid,
        descriptor.heapName!,
        [
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
            name: 'Total Size',
            unit: 'B',
            columnName: 'self_alloc_size',
          },
          {
            name: 'Total Count',
            unit: '',
            columnName: 'self_alloc_count',
          },
        ],
      );
    case ProfileType.JAVA_HEAP_SAMPLES:
      return flamegraphMetricsForHeapProfile(
        ts,
        tsEnd,
        upid,
        descriptor.heapName!,
        [
          {
            name: 'Total Allocation Size',
            unit: 'B',
            columnName: 'self_size',
          },
          {
            name: 'Total Allocation Count',
            unit: '',
            columnName: 'self_count',
          },
        ],
      );
    case ProfileType.JAVA_HEAP_GRAPH:
      return [
        {
          name: 'Object Size',
          unit: 'B',
          dependencySql:
            'include perfetto module android.memory.heap_graph.class_tree;',
          statement: `
            select
              id,
              parent_id as parentId,
              ifnull(name, 'unknown') as name,
              root_type,
              heap_type,
              self_size as value,
              self_count,
              path_hash_stable
            from _heap_graph_class_tree
            where graph_sample_ts = ${ts} and upid = ${upid}
          `,
          unaggregatableProperties: [
            {name: 'root_type', displayName: 'Root Type'},
            {name: 'heap_type', displayName: 'Heap Type'},
          ],
          aggregatableProperties: [
            {
              name: 'self_count',
              displayName: 'Self Count',
              mergeAggregation: 'SUM',
            },
            {
              name: 'path_hash_stable',
              displayName: 'Path Hash',
              mergeAggregation: 'CONCAT_WITH_COMMA',
              isVisible: (_) => false,
            },
          ],
          optionalNodeActions: getHeapGraphNodeOptionalActions(
            trace,
            false,
            onNodeSelected,
          ),
          optionalRootActions: getHeapGraphRootOptionalActions(trace, false),
        },
        {
          name: 'Object Count',
          unit: '',
          dependencySql:
            'include perfetto module android.memory.heap_graph.class_tree;',
          statement: `
            select
              id,
              parent_id as parentId,
              ifnull(name, 'unknown') as name,
              root_type,
              heap_type,
              self_size,
              self_count as value,
              path_hash_stable
            from _heap_graph_class_tree
            where graph_sample_ts = ${ts} and upid = ${upid}
          `,
          unaggregatableProperties: [
            {name: 'root_type', displayName: 'Root Type'},
            {name: 'heap_type', displayName: 'Heap Type'},
          ],
          aggregatableProperties: [
            {
              name: 'path_hash_stable',
              displayName: 'Path Hash',
              mergeAggregation: 'CONCAT_WITH_COMMA',
              isVisible: (_) => false,
            },
          ],
          optionalNodeActions: getHeapGraphNodeOptionalActions(
            trace,
            false,
            onNodeSelected,
          ),
          optionalRootActions: getHeapGraphRootOptionalActions(trace, false),
        },
        {
          name: 'Dominated Object Size',
          unit: 'B',
          dependencySql:
            'include perfetto module android.memory.heap_graph.dominator_class_tree;',
          statement: `
            select
              id,
              parent_id as parentId,
              ifnull(name, 'unknown') as name,
              root_type,
              heap_type,
              self_size as value,
              self_count,
              path_hash_stable
            from _heap_graph_dominator_class_tree
            where graph_sample_ts = ${ts} and upid = ${upid}
          `,
          unaggregatableProperties: [
            {name: 'root_type', displayName: 'Root Type'},
            {name: 'heap_type', displayName: 'Heap Type'},
          ],
          aggregatableProperties: [
            {
              name: 'self_count',
              displayName: 'Self Count',
              mergeAggregation: 'SUM',
            },
            {
              name: 'path_hash_stable',
              displayName: 'Path Hash',
              mergeAggregation: 'CONCAT_WITH_COMMA',
              isVisible: (_) => false,
            },
          ],
          optionalNodeActions: getHeapGraphNodeOptionalActions(
            trace,
            true,
            onNodeSelected,
          ),
          optionalRootActions: getHeapGraphRootOptionalActions(trace, true),
        },
        {
          name: 'Dominated Object Count',
          unit: '',
          dependencySql:
            'include perfetto module android.memory.heap_graph.dominator_class_tree;',
          statement: `
            select
              id,
              parent_id as parentId,
              ifnull(name, 'unknown') as name,
              root_type,
              heap_type,
              self_size,
              self_count as value,
              path_hash_stable
            from _heap_graph_dominator_class_tree
            where graph_sample_ts = ${ts} and upid = ${upid}
          `,
          unaggregatableProperties: [
            {name: 'root_type', displayName: 'Root Type'},
            {name: 'heap_type', displayName: 'Heap Type'},
          ],
          aggregatableProperties: [
            {
              name: 'path_hash_stable',
              displayName: 'Path Hash',
              mergeAggregation: 'CONCAT_WITH_COMMA',
              isVisible: (_) => false,
            },
          ],
          optionalNodeActions: getHeapGraphNodeOptionalActions(
            trace,
            true,
            onNodeSelected,
          ),
          optionalRootActions: getHeapGraphRootOptionalActions(trace, true),
        },
      ];
    case ProfileType.OOME_CALLSTACK:
      return buildOomeCallstackMetrics(ts);
  }
}

function flamegraphMetricsForHeapProfile(
  ts: time,
  tsEnd: bigint,
  upid: number,
  heapName: string,
  metrics: {name: string; unit: string; columnName: string}[],
) {
  return metricsFromTableOrSubquery({
    tableOrSubquery: `
      (
        -- Any selection overlap with an allocation slice includes the
        -- slice in the result. Practically this means that we might need to
        -- extend the right-side boundary.
        with alloc_bound as (
          select ts
          from heap_profile_allocation
          where ts >= ${tsEnd}
            and upid = ${upid} and heap_name = '${heapName}'
          order by ts asc
          limit 1
        ),
        -- The native heap profiler data model is delta-encoded.
        -- Unreleased allocations will be recorded across continuous dumps
        -- and trace processor is responsible for deduplicating them.
        -- If an allocation at ts1 is released in ts2, this will
        -- be represented as an unmatched memory released in ts2 (negative size).
        -- For the purposes of looking at ts2+ slices, we need to ignore
        -- the negative sized data points.
        alloc_class as (
          select callsite_id, if(sum(count) > 0, 1, 0) as positive_alloc
          from heap_profile_allocation
          where ts >= ${ts} and ts <= ifnull((SELECT ts FROM alloc_bound), ${tsEnd})
            and upid = ${upid} and heap_name = '${heapName}'
          group by callsite_id
        )
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_size,
          self_count,
          self_alloc_size,
          self_alloc_count
        from _android_heap_profile_callstacks_for_allocations!((
          select
            callsite_id,
            iif(positive_alloc, size, 0) as size,
            iif(positive_alloc, count, 0) as count,
            max(size, 0) as alloc_size,
            max(count, 0) as alloc_count
          from heap_profile_allocation a
          join alloc_class using (callsite_id)
          where a.ts >= ${ts} and a.ts <= ifnull((SELECT ts FROM alloc_bound), ${tsEnd})
            and a.upid = ${upid} and a.heap_name = '${heapName}'
        ))
      )
    `,
    tableMetrics: metrics,
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

// The traceconv conversion mode for a profile type, or undefined for the
// types traceconv cannot emit as pprof. All heapprofd-backed heaps (native,
// ART samples and custom allocators) are allocator profiles; the ART heap
// dump is a heap graph. The OOME callstack is a single stack rather than a
// profile, so it has no pprof of its own.
function pprofProfileType(type: ProfileType): PprofProfileType | undefined {
  switch (type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
    case ProfileType.JAVA_HEAP_SAMPLES:
    case ProfileType.GENERIC_HEAP_PROFILE:
      return 'alloc';
    case ProfileType.JAVA_HEAP_GRAPH:
      return 'java-heap';
    case ProfileType.OOME_CALLSTACK:
      return undefined;
  }
}

async function downloadPprof(
  trace: Trace,
  upid: number,
  ts: time,
  profileType: PprofProfileType,
) {
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
    profileType,
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

function getHeapGraphDuplicateObjectsView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}duplicate_objects`,
    columns: [
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'path_count', type: PerfettoSqlTypes.INT},
      {column: 'object_count', type: PerfettoSqlTypes.INT},
      {column: 'total_size', type: PerfettoSqlTypes.INT},
      {column: 'total_native_size', type: PerfettoSqlTypes.INT},
    ],
  };
}

function getHeapGraphNodeOptionalActions(
  trace: Trace,
  isDominator: boolean,
  onNodeSelected?: (pathHashes: string, isDominator: boolean) => void,
): ReadonlyArray<TreeExplorerOptionalAction> {
  if (!trace.plugins.isPluginEnabled('com.android.HeapDumpExplorer')) {
    return [];
  }
  return [
    {
      name: 'Open in Heapdump Explorer',
      icon: 'open_in_new',
      category: 'DRILL',
      description:
        "Inspect this class's retained objects in the Heap Dump Explorer.",
      execute: async ({properties, node}) => {
        const pathHashes = properties.get('path_hash_stable');
        if (pathHashes === undefined) return;

        onNodeSelected?.(pathHashes, isDominator);

        const name = node?.name;
        const nameSuffix =
          name !== undefined ? `_${encodeURIComponent(name)}` : '';
        trace.navigate(`#!/heapdump/flamegraph_objects${nameSuffix}`);
      },
    },
  ];
}

function getHeapGraphRootOptionalActions(
  trace: Trace,
  isDominator: boolean,
): ReadonlyArray<TreeExplorerOptionalAction> {
  return [
    {
      name: 'Reference paths by class',
      icon: 'account_tree',
      description: 'Group duplicate reference paths by class in a table.',
      execute: async () => {
        const viewName = `_heap_graph${tableModifier(isDominator)}duplicate_objects`;
        const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes`;
        const macroExpr = `_heap_graph_duplicate_objects_agg!(${macroArgs})`;
        const statement = `CREATE OR REPLACE PERFETTO VIEW ${viewName} AS SELECT * FROM ${macroExpr};`;

        // Create view to be returned
        await trace.engine.query(statement);
        extensions.addLegacySqlTableTab(trace, {
          table: getHeapGraphDuplicateObjectsView(isDominator),
        });
      },
    },
  ];
}

function tableModifier(isDominator: boolean): string {
  return isDominator ? '_dominator_' : '_';
}
