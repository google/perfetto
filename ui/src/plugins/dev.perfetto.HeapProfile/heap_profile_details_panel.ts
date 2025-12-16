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

import {assertFalse} from '../../base/logging';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {extensions} from '../../components/extensions';
import {time} from '../../base/time';
import {uuidv4Sql} from '../../base/uuid';
import {
  QueryFlamegraph,
  QueryFlamegraphMetric,
  metricsFromTableOrSubquery,
} from '../../components/query_flamegraph';
import {convertTraceToPprofAndDownload} from '../../frontend/trace_converter';
import {Timestamp} from '../../components/widgets/timestamp';
import {
  TrackEventDetailsPanel,
  TrackEventDetailsPanelSerializeArgs,
} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {Icon} from '../../widgets/icon';
import {Modal, showModal} from '../../widgets/modal';
import {
  Flamegraph,
  FlamegraphState,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphOptionalAction,
} from '../../widgets/flamegraph';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {Stack} from '../../widgets/stack';
import {Tooltip} from '../../widgets/tooltip';

export enum ProfileType {
  HEAP_PROFILE = 'heap_profile',
  MIXED_HEAP_PROFILE = 'heap_profile:com.android.art,libc.malloc',
  NATIVE_HEAP_PROFILE = 'heap_profile:libc.malloc',
  JAVA_HEAP_SAMPLES = 'heap_profile:com.android.art',
  JAVA_HEAP_GRAPH = 'graph',
  PERF_SAMPLE = 'perf',
  INSTRUMENTS_SAMPLE = 'instruments',
}

export function profileType(s: string): ProfileType {
  if (s === 'heap_profile:libc.malloc,com.android.art') {
    s = 'heap_profile:com.android.art,libc.malloc';
  }
  if (Object.values(ProfileType).includes(s as ProfileType)) {
    return s as ProfileType;
  }
  if (s.startsWith('heap_profile')) {
    return ProfileType.HEAP_PROFILE;
  }
  throw new Error('Unknown type ${s}');
}

interface Props {
  ts: time;
  type: ProfileType;
}

export class HeapProfileFlamegraphDetailsPanel
  implements TrackEventDetailsPanel
{
  private flamegraph: QueryFlamegraph;
  private readonly props: Props;
  private flamegraphModalDismissed = false;

  // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
  // We moved serialization from being attached to selections to instead being
  // attached to the plugin that loaded the panel.
  readonly serialization: TrackEventDetailsPanelSerializeArgs<
    FlamegraphState | undefined
  > = {
    schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
    state: undefined,
  };

  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;

  constructor(
    private readonly trace: Trace,
    private readonly heapGraphIncomplete: boolean,
    private readonly upid: number,
    private readonly profileType: ProfileType,
    private readonly ts: time,
    private state: FlamegraphState | undefined,
    private readonly onStateChange: (state: FlamegraphState) => void,
  ) {
    this.props = {ts, type: profileType};
    this.flamegraph = new QueryFlamegraph(trace);
    this.metrics = flamegraphMetrics(
      this.trace,
      this.profileType,
      this.ts,
      this.upid,
    );
    if (this.state === undefined) {
      this.state = Flamegraph.createDefaultState(this.metrics);
      onStateChange(this.state);
    }
  }

  async load() {
    // If the state in the serialization is not undefined, we should read from
    // it.
    // TODO(lalitm): remove this in 26Q2 - see comment on `serialization`.
    if (this.serialization.state !== undefined) {
      this.state = Flamegraph.updateState(
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
      '.pf-flamegraph-profile',
      this.maybeShowModal(this.trace, type, this.heapGraphIncomplete),
      m(
        DetailsShell,
        {
          fillHeight: true,
          title: m(
            'span',
            getFlamegraphTitle(type),
            type === ProfileType.MIXED_HEAP_PROFILE && [
              ' ', // Some space between title and icon
              m(
                Tooltip,
                {
                  trigger: m(Icon, {icon: 'warning', intent: Intent.Warning}),
                },
                m(
                  '',
                  'This is a mixed java/native heap profile, free()s are not visualized. To visualize free()s, remove "all_heaps: true" from the config.',
                ),
              ),
            ],
          ),
          buttons: m(Stack, {orientation: 'horizontal', spacing: 'large'}, [
            m('span', `Snapshot time: `, m(Timestamp, {trace: this.trace, ts})),
            (type === ProfileType.NATIVE_HEAP_PROFILE ||
              type === ProfileType.JAVA_HEAP_SAMPLES) &&
              m(Button, {
                icon: 'file_download',
                intent: Intent.Primary,
                variant: ButtonVariant.Filled,
                onclick: () => {
                  downloadPprof(this.trace, this.upid, ts);
                },
              }),
          ]),
        },
        this.flamegraph.render({
          metrics: this.metrics,
          state: this.state,
          onStateChange: (state) => {
            this.state = state;
            this.onStateChange(state);
          },
        }),
      ),
    );
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
    return m(Modal, {
      title: 'The flamegraph is incomplete',
      vAlign: 'TOP',
      content: m(
        'div',
        'The current trace does not have a fully formed flamegraph',
      ),
      buttons: [
        {
          text: 'Show the errors',
          primary: true,
          action: () => trace.navigate('#!/info'),
        },
        {
          text: 'Skip',
          action: () => {
            this.flamegraphModalDismissed = true;
          },
        },
      ],
    });
  }
}

function flamegraphMetrics(
  trace: Trace,
  type: ProfileType,
  ts: time,
  upid: number,
): ReadonlyArray<QueryFlamegraphMetric> {
  switch (type) {
    case ProfileType.NATIVE_HEAP_PROFILE:
      return flamegraphMetricsForHeapProfile(ts, upid, [
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
      ]);
    case ProfileType.HEAP_PROFILE:
      return flamegraphMetricsForHeapProfile(ts, upid, [
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
      ]);
    case ProfileType.JAVA_HEAP_SAMPLES:
      return flamegraphMetricsForHeapProfile(ts, upid, [
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
      ]);
    case ProfileType.MIXED_HEAP_PROFILE:
      return flamegraphMetricsForHeapProfile(ts, upid, [
        {
          name: 'Allocation Size (malloc + java)',
          unit: 'B',
          columnName: 'self_size',
        },
        {
          name: 'Allocation Count (malloc + java)',
          unit: '',
          columnName: 'self_count',
        },
      ]);
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
              ifnull(name, '[Unknown]') as name,
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
          optionalNodeActions: getHeapGraphNodeOptionalActions(trace, false),
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
              ifnull(name, '[Unknown]') as name,
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
          optionalNodeActions: getHeapGraphNodeOptionalActions(trace, false),
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
              ifnull(name, '[Unknown]') as name,
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
          optionalNodeActions: getHeapGraphNodeOptionalActions(trace, true),
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
              ifnull(name, '[Unknown]') as name,
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
          optionalNodeActions: getHeapGraphNodeOptionalActions(trace, true),
          optionalRootActions: getHeapGraphRootOptionalActions(trace, true),
        },
      ];
    case ProfileType.PERF_SAMPLE:
      throw new Error('Perf sample not supported');
    case ProfileType.INSTRUMENTS_SAMPLE:
      throw new Error('Instruments sample not supported');
  }
}

function flamegraphMetricsForHeapProfile(
  ts: time,
  upid: number,
  metrics: {name: string; unit: string; columnName: string}[],
) {
  return metricsFromTableOrSubquery(
    `
      (
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
            size,
            count,
            max(size, 0) as alloc_size,
            max(count, 0) as alloc_count
          from heap_profile_allocation a
          where a.ts <= ${ts} and a.upid = ${upid}
        ))
      )
    `,
    metrics,
    'include perfetto module android.memory.heap_profile.callstacks',
    [{name: 'mapping_name', displayName: 'Mapping'}],
    [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
  );
}

function getFlamegraphTitle(type: ProfileType) {
  switch (type) {
    case ProfileType.HEAP_PROFILE:
      return 'Heap profile';
    case ProfileType.JAVA_HEAP_GRAPH:
      return 'Java heap graph';
    case ProfileType.JAVA_HEAP_SAMPLES:
      return 'Java heap samples';
    case ProfileType.MIXED_HEAP_PROFILE:
      return 'Mixed heap profile';
    case ProfileType.NATIVE_HEAP_PROFILE:
      return 'Native heap profile';
    case ProfileType.PERF_SAMPLE:
      assertFalse(false, 'Perf sample not supported');
      return 'Impossible';
    case ProfileType.INSTRUMENTS_SAMPLE:
      assertFalse(false, 'Instruments sample not supported');
      return 'Impossible';
  }
}

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
  convertTraceToPprofAndDownload(blob, pid.firstRow({pid: NUM}).pid, ts);
}

function getHeapGraphObjectReferencesView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}object_references`,
    columns: [
      {column: 'path_hash', type: PerfettoSqlTypes.STRING},
      {column: 'outgoing_reference_count', type: PerfettoSqlTypes.INT},
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'self_size', type: PerfettoSqlTypes.INT},
      {column: 'native_size', type: PerfettoSqlTypes.INT},
      {column: 'heap_type', type: PerfettoSqlTypes.STRING},
      {column: 'root_type', type: PerfettoSqlTypes.STRING},
      {column: 'reachable', type: PerfettoSqlTypes.BOOLEAN},
    ],
  };
}

function getHeapGraphIncomingReferencesView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}incoming_references`,
    columns: [
      {column: 'path_hash', type: PerfettoSqlTypes.STRING},
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'field_name', type: PerfettoSqlTypes.STRING},
      {column: 'field_type_name', type: PerfettoSqlTypes.STRING},
      {column: 'self_size', type: PerfettoSqlTypes.INT},
      {column: 'native_size', type: PerfettoSqlTypes.INT},
      {column: 'heap_type', type: PerfettoSqlTypes.STRING},
      {column: 'root_type', type: PerfettoSqlTypes.STRING},
      {column: 'reachable', type: PerfettoSqlTypes.BOOLEAN},
    ],
  };
}

function getHeapGraphOutgoingReferencesView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}outgoing_references`,
    columns: [
      {column: 'path_hash', type: PerfettoSqlTypes.STRING},
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'field_name', type: PerfettoSqlTypes.STRING},
      {column: 'field_type_name', type: PerfettoSqlTypes.STRING},
      {column: 'self_size', type: PerfettoSqlTypes.INT},
      {column: 'native_size', type: PerfettoSqlTypes.INT},
      {column: 'heap_type', type: PerfettoSqlTypes.STRING},
      {column: 'root_type', type: PerfettoSqlTypes.STRING},
      {column: 'reachable', type: PerfettoSqlTypes.BOOLEAN},
    ],
  };
}

function getHeapGraphRetainingObjectCountsView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}retaining_object_counts`,
    columns: [
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'count', type: PerfettoSqlTypes.INT},
      {column: 'total_size', type: PerfettoSqlTypes.INT},
      {column: 'total_native_size', type: PerfettoSqlTypes.INT},
      {column: 'heap_type', type: PerfettoSqlTypes.STRING},
      {column: 'root_type', type: PerfettoSqlTypes.STRING},
      {column: 'reachable', type: PerfettoSqlTypes.BOOLEAN},
    ],
  };
}

function getHeapGraphRetainedObjectCountsView(
  isDominator: boolean,
): SqlTableDefinition {
  return {
    name: `_heap_graph${tableModifier(isDominator)}retained_object_counts`,
    columns: [
      {column: 'class_name', type: PerfettoSqlTypes.STRING},
      {column: 'count', type: PerfettoSqlTypes.INT},
      {column: 'total_size', type: PerfettoSqlTypes.INT},
      {column: 'total_native_size', type: PerfettoSqlTypes.INT},
      {column: 'heap_type', type: PerfettoSqlTypes.STRING},
      {column: 'root_type', type: PerfettoSqlTypes.STRING},
      {column: 'reachable', type: PerfettoSqlTypes.BOOLEAN},
    ],
  };
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
): ReadonlyArray<FlamegraphOptionalAction> {
  return [
    {
      name: 'Objects',
      execute: async (kv: ReadonlyMap<string, string>) => {
        const value = kv.get('path_hash_stable');
        if (value !== undefined) {
          const uuid = uuidv4Sql();
          const pathHashTableName = `_heap_graph_filtered_path_hashes_${uuid}`;
          await createPerfettoTable({
            engine: trace.engine,
            name: pathHashTableName,
            as: pathHashesToTableStatement(value),
          });

          const tableName = `_heap_graph${tableModifier(isDominator)}object_references`;
          const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes, ${pathHashTableName}`;
          const macroExpr = `_heap_graph_object_references_agg!(${macroArgs})`;
          const statement = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS SELECT * FROM ${macroExpr};`;

          // Create view to be returned
          await trace.engine.query(statement);
          extensions.addLegacySqlTableTab(trace, {
            table: getHeapGraphObjectReferencesView(isDominator),
          });
        }
      },
    },

    // Group for Direct References
    {
      name: 'Direct References',
      // No execute function for parent menu items
      subActions: [
        {
          name: 'Incoming references',
          execute: async (kv: ReadonlyMap<string, string>) => {
            const value = kv.get('path_hash_stable');
            if (value !== undefined) {
              const uuid = uuidv4Sql();
              const pathHashTableName = `_heap_graph_filtered_path_hashes_${uuid}`;
              await createPerfettoTable({
                engine: trace.engine,
                name: pathHashTableName,
                as: pathHashesToTableStatement(value),
              });

              const tableName = `_heap_graph${tableModifier(isDominator)}incoming_references`;
              const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes, ${pathHashTableName}`;
              const macroExpr = `_heap_graph_incoming_references_agg!(${macroArgs})`;
              const statement = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS SELECT * FROM ${macroExpr};`;

              // Create view to be returned
              await trace.engine.query(statement);
              extensions.addLegacySqlTableTab(trace, {
                table: getHeapGraphIncomingReferencesView(isDominator),
              });
            }
          },
        },
        {
          name: 'Outgoing references',
          execute: async (kv: ReadonlyMap<string, string>) => {
            const value = kv.get('path_hash_stable');
            if (value !== undefined) {
              const uuid = uuidv4Sql();
              const pathHashTableName = `_heap_graph_filtered_path_hashes_${uuid}`;
              await createPerfettoTable({
                engine: trace.engine,
                name: pathHashTableName,
                as: pathHashesToTableStatement(value),
              });

              const tableName = `_heap_graph${tableModifier(isDominator)}outgoing_references`;
              const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes, ${pathHashTableName}`;
              const macroExpr = `_heap_graph_outgoing_references_agg!(${macroArgs})`;
              const statement = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS SELECT * FROM ${macroExpr};`;

              // Create view to be returned
              await trace.engine.query(statement);
              extensions.addLegacySqlTableTab(trace, {
                table: getHeapGraphOutgoingReferencesView(isDominator),
              });
            }
          },
        },
      ],
    },

    // Group for Indirect References
    {
      name: 'Indirect References',
      // No execute function for parent menu items
      subActions: [
        {
          name: 'Retained objects',
          execute: async (kv: ReadonlyMap<string, string>) => {
            const value = kv.get('path_hash_stable');
            if (value !== undefined) {
              const uuid = uuidv4Sql();
              const pathHashTableName = `_heap_graph_filtered_path_hashes_${uuid}`;
              await createPerfettoTable({
                engine: trace.engine,
                name: pathHashTableName,
                as: pathHashesToTableStatement(value),
              });

              const tableName = `_heap_graph${tableModifier(isDominator)}retained_object_counts`;
              const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes, ${pathHashTableName}`;
              const macroExpr = `_heap_graph_retained_object_count_agg!(${macroArgs})`;
              const statement = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS SELECT * FROM ${macroExpr};`;

              // Create view to be returned
              await trace.engine.query(statement);
              extensions.addLegacySqlTableTab(trace, {
                table: getHeapGraphRetainedObjectCountsView(isDominator),
              });
            }
          },
        },
        {
          name: 'Retaining objects',
          execute: async (kv: ReadonlyMap<string, string>) => {
            const value = kv.get('path_hash_stable');
            if (value !== undefined) {
              const uuid = uuidv4Sql();
              const pathHashTableName = `_heap_graph_filtered_path_hashes_${uuid}`;
              await createPerfettoTable({
                engine: trace.engine,
                name: pathHashTableName,
                as: pathHashesToTableStatement(value),
              });

              const tableName = `_heap_graph${tableModifier(isDominator)}retaining_object_counts`;
              const macroArgs = `_heap_graph${tableModifier(isDominator)}path_hashes, ${pathHashTableName}`;
              const macroExpr = `_heap_graph_retaining_object_count_agg!(${macroArgs})`;
              const statement = `CREATE OR REPLACE PERFETTO TABLE ${tableName} AS SELECT * FROM ${macroExpr};`;

              // Create view to be returned
              await trace.engine.query(statement);
              extensions.addLegacySqlTableTab(trace, {
                table: getHeapGraphRetainingObjectCountsView(isDominator),
              });
            }
          },
        },
      ],
    },
  ];
}

function getHeapGraphRootOptionalActions(
  trace: Trace,
  isDominator: boolean,
): ReadonlyArray<FlamegraphOptionalAction> {
  return [
    {
      name: 'Reference paths by class',
      execute: async (_kv: ReadonlyMap<string, string>) => {
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

function pathHashesToTableStatement(commaSeparatedValues: string): string {
  // Split the string by commas and trim whitespace
  const individualValues = commaSeparatedValues.split(',').map((v) => v.trim());

  // Wrap each value with parentheses
  const wrappedValues = individualValues.map((value) => `(${value})`);

  // Join with commas and create the complete WITH clause
  const valuesClause = `values${wrappedValues.join(', ')}`;
  return `WITH temp_table(path_hash) AS (${valuesClause}) SELECT * FROM temp_table`;
}
