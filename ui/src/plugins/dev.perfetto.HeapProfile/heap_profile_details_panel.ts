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

import {assertExists, assertFalse} from '../../base/logging';
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
import {Popup} from '../../widgets/popup';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphState,
  FlamegraphOptionalAction,
} from '../../widgets/flamegraph';
import {SqlTableDescription} from '../../components/widgets/sql/table/table_description';
import {StandardColumn} from '../../components/widgets/sql/table/columns';

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
  private readonly flamegraph: QueryFlamegraph;
  private readonly props: Props;
  private flamegraphModalDismissed = false;

  readonly serialization: TrackEventDetailsPanelSerializeArgs<FlamegraphState>;

  constructor(
    private trace: Trace,
    private heapGraphIncomplete: boolean,
    private upid: number,
    profileType: ProfileType,
    ts: time,
  ) {
    const metrics = flamegraphMetrics(trace, profileType, ts, upid);
    this.serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(metrics),
    };
    this.flamegraph = new QueryFlamegraph(trace, metrics, this.serialization);
    this.props = {ts, type: profileType};
  }

  render() {
    const {type, ts} = this.props;
    return m(
      '.flamegraph-profile',
      this.maybeShowModal(this.trace, type, this.heapGraphIncomplete),
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m(
            '.title',
            getFlamegraphTitle(type),
            type === ProfileType.MIXED_HEAP_PROFILE &&
              m(
                Popup,
                {
                  trigger: m(Icon, {icon: 'warning'}),
                },
                m(
                  '',
                  {style: {width: '300px'}},
                  'This is a mixed java/native heap profile, free()s are not visualized. To visualize free()s, remove "all_heaps: true" from the config.',
                ),
              ),
          ),
          description: [],
          buttons: [
            m('.time', `Snapshot time: `, m(Timestamp, {ts})),
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
          ],
        },
        assertExists(this.flamegraph).render(),
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
              isVisible: false,
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
              isVisible: false,
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
              isVisible: false,
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
              isVisible: false,
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
          source_file,
          cast(line_number AS text) as line_number,
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
        name: 'source_file',
        displayName: 'Source File',
        mergeAggregation: 'ONE_OR_NULL',
      },
      {
        name: 'line_number',
        displayName: 'Line Number',
        mergeAggregation: 'ONE_OR_NULL',
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
  }
  const blob = await trace.getTraceFile();
  convertTraceToPprofAndDownload(blob, pid.firstRow({pid: NUM}).pid, ts);
}

function getHeapGraphObjectReferencesView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}object_references`,
    columns: [
      new StandardColumn('path_hash'),
      new StandardColumn('outgoing_reference_count'),
      new StandardColumn('class_name'),
      new StandardColumn('self_size'),
      new StandardColumn('native_size'),
      new StandardColumn('heap_type'),
      new StandardColumn('root_type'),
      new StandardColumn('reachable'),
    ],
  };
}

function getHeapGraphIncomingReferencesView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}incoming_references`,
    columns: [
      new StandardColumn('path_hash'),
      new StandardColumn('class_name'),
      new StandardColumn('field_name'),
      new StandardColumn('field_type_name'),
      new StandardColumn('self_size'),
      new StandardColumn('native_size'),
      new StandardColumn('heap_type'),
      new StandardColumn('root_type'),
      new StandardColumn('reachable'),
    ],
  };
}

function getHeapGraphOutgoingReferencesView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}outgoing_references`,
    columns: [
      new StandardColumn('path_hash'),
      new StandardColumn('class_name'),
      new StandardColumn('field_name'),
      new StandardColumn('field_type_name'),
      new StandardColumn('self_size'),
      new StandardColumn('native_size'),
      new StandardColumn('heap_type'),
      new StandardColumn('root_type'),
      new StandardColumn('reachable'),
    ],
  };
}

function getHeapGraphRetainingObjectCountsView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}retaining_object_counts`,
    columns: [
      new StandardColumn('class_name'),
      new StandardColumn('count'),
      new StandardColumn('total_size'),
      new StandardColumn('total_native_size'),
      new StandardColumn('heap_type'),
      new StandardColumn('root_type'),
      new StandardColumn('reachable'),
    ],
  };
}

function getHeapGraphRetainedObjectCountsView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}retained_object_counts`,
    columns: [
      new StandardColumn('class_name'),
      new StandardColumn('count'),
      new StandardColumn('total_size'),
      new StandardColumn('total_native_size'),
      new StandardColumn('heap_type'),
      new StandardColumn('root_type'),
      new StandardColumn('reachable'),
    ],
  };
}

function getHeapGraphDuplicateObjectsView(
  isDominator: boolean,
): SqlTableDescription {
  return {
    name: `_heap_graph${tableModifier(isDominator)}duplicate_objects`,
    columns: [
      new StandardColumn('class_name'),
      new StandardColumn('path_count'),
      new StandardColumn('object_count'),
      new StandardColumn('total_size'),
      new StandardColumn('total_native_size'),
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
          await createPerfettoTable(
            trace.engine,
            pathHashTableName,
            pathHashesToTableStatement(value),
          );

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
              await createPerfettoTable(
                trace.engine,
                pathHashTableName,
                pathHashesToTableStatement(value),
              );

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
              await createPerfettoTable(
                trace.engine,
                pathHashTableName,
                pathHashesToTableStatement(value),
              );

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
              await createPerfettoTable(
                trace.engine,
                pathHashTableName,
                pathHashesToTableStatement(value),
              );

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
              await createPerfettoTable(
                trace.engine,
                pathHashTableName,
                pathHashesToTableStatement(value),
              );

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
