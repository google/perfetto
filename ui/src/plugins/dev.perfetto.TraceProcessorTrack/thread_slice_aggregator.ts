// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the \"License\");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an \"AS IS\" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {Time} from '../../base/time';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
  createIITable,
} from '../../components/aggregation_adapter';
import {Timestamp} from '../../components/widgets/timestamp';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {SourceDataset} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import type {SQLTableSchema} from '../../components/widgets/datagrid/sql_schema';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import {Anchor} from '../../widgets/anchor';
import {formatDurationValue} from '../../components/aggregation_panel';
import {getOrCreate} from '../../base/utils';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';

const TRACK_SCHEMA: ColumnSchema = {
  id: {
    title: 'ID',
    columnType: 'identifier',
  },
  name: {
    title: 'Name',
    columnType: 'text',
  },
  type: {
    title: 'Type',
    columnType: 'text',
  },
  dimension_arg_set_id: {
    title: 'Dimension Arg Set ID',
    parameterized: true,
  },
  parent_id: {
    title: 'Parent',
    get schema() {
      return TRACK_SCHEMA;
    },
  },
  source_arg_set_id: {
    title: 'Source Arg Set ID',
    parameterized: true,
  },
  machine_id: {
    title: 'Machine ID',
    columnType: 'identifier',
  },
  track_group_id: {
    title: 'Track Group ID',
    columnType: 'identifier',
  },
};

const TRACK_SQL_SCHEMA: SQLTableSchema = {
  tableOrSubquery: 'track',
  columns: {
    dimension_arg_set_id: {
      expression: (alias, key) =>
        `extract_arg(${alias}.dimension_arg_set_id, '${key}')`,
      parameterized: true,
      parameterKeysQuery: (tableOrSubquery, alias) => `
        SELECT DISTINCT args.key
        FROM (${tableOrSubquery}) AS ${alias}
        JOIN args ON args.arg_set_id = ${alias}.dimension_arg_set_id
        WHERE args.key IS NOT NULL
        ORDER BY args.key
        LIMIT 1000
      `,
    },
    parent_id: {
      foreignKey: 'parent_id',
      get schema() {
        return TRACK_SQL_SCHEMA;
      },
    },
    source_arg_set_id: {
      expression: (alias, key) =>
        `extract_arg(${alias}.source_arg_set_id, '${key}')`,
      parameterized: true,
      parameterKeysQuery: (tableOrSubquery, alias) => `
        SELECT DISTINCT args.key
        FROM (${tableOrSubquery}) AS ${alias}
        JOIN args ON args.arg_set_id = ${alias}.source_arg_set_id
        WHERE args.key IS NOT NULL
        ORDER BY args.key
        LIMIT 1000
      `,
    },
  },
};

export class ThreadSliceAggregator implements Aggregator {
  readonly id = 'thread_slice_aggregator';

  private readonly trace: Trace;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  probe(area: AreaSelection): Aggregation | undefined {
    // Collect up all tracks with the slice kind and a trackIds tag, retaining a mapping from trackId -> tag
    const trackIdMap = new Map<number, Track[]>();

    // Iterate all selected tracks and index tracks by track id
    for (const track of area.tracks) {
      const tags = track.tags;
      const kinds = tags?.kinds;
      const trackIds = tags?.trackIds;

      if (kinds?.includes(SLICE_TRACK_KIND) && trackIds) {
        for (const trackId of trackIds) {
          const bucket = getOrCreate(trackIdMap, trackId, () => []);
          bucket.push(track);
        }
      }
    }

    // No matching tracks - return undefined to hide this aggregation
    if (trackIdMap.size === 0) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(trackIdMap),
      prepareData: async (engine: Engine) => {
        await using iiTable = await this.buildSliceQuery(
          engine,
          Array.from(trackIdMap.keys()),
          area,
        );

        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              json_object('id', id, 'trackId', track_id) as id_with_lineage,
              name,
              ts,
              dur,
              self_dur,
              depth,
              parent_id,
              arg_set_id,
              track_id,
              utid,
              upid
            FROM (${iiTable.name})
          `,
        });

        return createAggregationData(table);
      },
    };
  }

  private async buildSliceQuery(
    engine: Engine,
    trackIds: readonly number[],
    area: AreaSelection,
  ) {
    // Create interval-intersect table for time filtering
    await using iiTable = await createIITable(
      engine,
      new SourceDataset({
        src: `
          SELECT * 
          FROM slice 
          WHERE track_id IN (${trackIds.join()})
        `,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          parent_id: NUM,
          arg_set_id: NUM,
          name: STR_NULL,
          track_id: NUM,
          depth: NUM,
        },
      }),
      area.start,
      area.end,
    );

    // Build child duration aggregation for self-time calculation
    await using childDurTable = await createPerfettoTable({
      engine,
      as: `
        SELECT
          parent_id AS id,
          SUM(dur) AS child_dur
        FROM ${iiTable.name}
        WHERE parent_id IS NOT NULL
        GROUP BY parent_id
      `,
    });

    // Create a coalesced table containing
    return await createPerfettoTable({
      engine,
      as: `
        SELECT
          slice.id AS id,
          slice.name as name,
          ts,
          dur,
          dur - COALESCE(child_dur, 0) AS self_dur,
          slice.depth AS depth,
          slice.parent_id AS parent_id,
          slice.arg_set_id AS arg_set_id,
          track_id,
          thread_track.utid AS utid,
          COALESCE(thread.upid, process_track.upid) AS upid
        FROM ${iiTable.name} AS slice
        LEFT JOIN ${childDurTable.name} USING (id)
        LEFT JOIN thread_track ON (slice.track_id = thread_track.id)
        LEFT JOIN thread USING (utid)
        LEFT JOIN process_track ON (slice.track_id = process_track.id)
      `,
    });
  }

  getTabName() {
    return 'Slices';
  }

  private getGridConfig(
    trackIdMap: Map<number, Track[]>,
  ): AggregatorGridConfig {
    const schema: ColumnSchema = {
      id_with_lineage: {
        title: 'ID',
        columnType: 'identifier',
        cellRenderer: (value: unknown) => {
          // Value is a JSON object {id, trackId}
          if (typeof value !== 'string') {
            return String(value);
          }

          const parsed = JSON.parse(value) as {
            id: number;
            trackId: number;
          };
          const {id, trackId} = parsed;

          // Resolve track from lineage
          const tracks = trackIdMap.get(trackId);
          if (!tracks) {
            return String(id);
          }

          if (tracks.length !== 1) {
            return m(
              PopupMenu,
              {
                trigger: m(
                  Anchor,
                  {
                    title: 'Go to slice',
                    icon: Icons.UpdateSelection,
                  },
                  String(id),
                ),
              },
              tracks.map((t) =>
                m(MenuItem, {
                  label: t.uri,
                  onclick: () => {
                    this.trace.selection.selectTrackEvent(t.uri, id, {
                      scrollToSelection: true,
                      switchToCurrentSelectionTab: false,
                    });
                  },
                }),
              ),
            );
          } else {
            const track = tracks[0];
            return m(
              Anchor,
              {
                title: 'Go to slice',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(track.uri, id, {
                    scrollToSelection: true,
                    switchToCurrentSelectionTab: false,
                  });
                },
              },
              String(id),
            );
          }
        },
      },
      name: {
        title: 'Name',
        columnType: 'text',
      },
      ts: {
        title: 'Timestamp',
        columnType: 'quantitative',
        cellRenderer: (value: unknown) => {
          if (typeof value === 'bigint') {
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
          }
          return String(value ?? '');
        },
      },
      dur: {
        title: 'Wall Duration',
        columnType: 'quantitative',
        cellRenderer: formatDurationValue,
      },
      self_dur: {
        title: 'Self Duration',
        columnType: 'quantitative',
        cellRenderer: formatDurationValue,
      },
      depth: {
        title: 'Depth',
        columnType: 'quantitative',
      },
      category: {
        title: 'Category',
        columnType: 'text',
      },
      parent_id: {
        title: 'Parent ID',
        columnType: 'identifier',
      },
      args: {
        title: 'Args',
        parameterized: true,
      },
      track: {
        title: 'Track',
        schema: TRACK_SCHEMA,
      },
      thread: {
        title: 'Thread',
        schema: {
          id: {
            title: 'UTID',
            columnType: 'identifier',
          },
          name: {
            title: 'Name',
            columnType: 'text',
          },
          tid: {
            title: 'TID',
            columnType: 'identifier',
          },
          upid: {
            title: 'UPID',
            columnType: 'identifier',
          },
          is_main_thread: {
            title: 'Is Main Thread',
            columnType: 'text',
          },
        },
      },
      process: {
        title: 'Process',
        schema: {
          id: {
            title: 'UPID',
            columnType: 'identifier',
          },
          name: {
            title: 'Name',
            columnType: 'text',
          },
          pid: {
            title: 'PID',
            columnType: 'identifier',
          },
          cmdline: {
            title: 'Cmdline',
            columnType: 'text',
          },
        },
      },
    };

    return {
      schema,
      // The aggregation table has an `arg_set_id` column, so we can expose a
      // parameterized `args.*` column to the datagrid.
      sqlConfig: ({sqlTable}): SQLTableSchema => ({
        tableOrSubquery: sqlTable.get().name,
        columns: {
          track: {
            foreignKey: 'track_id',
            schema: TRACK_SQL_SCHEMA,
          },
          thread: {
            foreignKey: 'utid',
            schema: {
              tableOrSubquery: 'thread',
              columns: {
                id: {},
                name: {},
                tid: {},
                upid: {},
                is_main_thread: {},
              },
            },
          },
          process: {
            foreignKey: 'upid',
            schema: {
              tableOrSubquery: 'process',
              columns: {
                id: {},
                name: {},
                pid: {},
                cmdline: {},
              },
            },
          },
          args: {
            expression: (alias, key) =>
              `extract_arg(${alias}.arg_set_id, '${key}')`,
            parameterized: true,
            parameterKeysQuery: (tableOrSubquery, alias) => `
                SELECT DISTINCT args.key
                FROM (${tableOrSubquery}) AS ${alias}
                JOIN args ON args.arg_set_id = ${alias}.arg_set_id
                WHERE args.key IS NOT NULL
                ORDER BY args.key
                LIMIT 1000
              `,
          },
        },
      }),
      initialColumns: [
        {id: 'id_with_lineage', field: 'id_with_lineage'},
        {id: 'ts', field: 'ts'},
        {id: 'dur', field: 'dur'},
        {id: 'name', field: 'name'},
        {id: 'self_dur', field: 'self_dur'},
        {id: 'arg_set_id', field: 'arg_set_id'},
      ],
      initialPivot: {
        groupBy: [{id: 'name', field: 'name'}],
        aggregates: [
          {id: 'count', function: 'COUNT'},
          {id: 'dur_sum', field: 'dur', function: 'SUM', sort: 'DESC'},
          {id: 'self_dur_sum', field: 'self_dur', function: 'SUM'},
          {id: 'dur_avg', field: 'dur', function: 'AVG'},
        ],
      },
    };
  }
}
