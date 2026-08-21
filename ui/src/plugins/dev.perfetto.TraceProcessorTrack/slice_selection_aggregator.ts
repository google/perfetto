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
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {Icons} from '../../base/semantic_icons';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createIITable,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {
  type Dataset,
  type DatasetSchema,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  type SqlValue,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import type {SQLTableSchema} from '../../components/widgets/datagrid/sql_schema';
import {Anchor} from '../../widgets/anchor';
import {formatDurationValue} from '../../components/aggregation_panel';

const SLICE_WITH_PARENT_SPEC = {
  id: NUM,
  name: STR_NULL,
  ts: LONG,
  dur: LONG,
  parent_id: NUM_NULL,
  arg_set_id: NUM_NULL,
};

const SLICELIKE_SPEC = {
  id: NUM,
  name: STR_NULL,
  ts: LONG,
  dur: LONG,
  arg_set_id: NUM_NULL,
};

export class SliceSelectionAggregator implements Aggregator {
  readonly id = 'slice_aggregation';

  private readonly trace: Trace;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  probe(area: AreaSelection): Aggregation | undefined {
    // Collect tracks with SourceDatasets, sorted by schema type
    const sliceTracks: Track[] = [];
    const slicelikeTracks: Track[] = [];

    for (const track of area.tracks) {
      const dataset = track.renderer.getDataset?.();
      if (!dataset || !(dataset instanceof SourceDataset)) continue;

      if (dataset.implements(SLICE_WITH_PARENT_SPEC)) {
        sliceTracks.push(track);
      } else if (dataset.implements(SLICELIKE_SPEC)) {
        slicelikeTracks.push(track);
      }
    }

    if (sliceTracks.length === 0 && slicelikeTracks.length === 0) {
      return undefined;
    }

    const unionQueries: string[] = [];
    const trackDatasetMap = new Map<Dataset, Track>();

    const sliceDatasets: Dataset[] = [];
    for (const track of sliceTracks) {
      const dataset = track.renderer.getDataset?.();
      if (dataset) {
        sliceDatasets.push(dataset);
        trackDatasetMap.set(dataset, track);
      }
    }
    const sliceUnionDataset =
      sliceDatasets.length > 0
        ? UnionDatasetWithLineage.create(sliceDatasets)
        : undefined;

    const slicelikeDatasets: Dataset[] = [];
    for (const track of slicelikeTracks) {
      const dataset = track.renderer.getDataset?.();
      if (dataset) {
        slicelikeDatasets.push(dataset);
        trackDatasetMap.set(dataset, track);
      }
    }
    const slicelikeUnionDataset =
      slicelikeDatasets.length > 0
        ? UnionDatasetWithLineage.create(slicelikeDatasets)
        : undefined;

    return {
      getGridConfig: () =>
        this.getGridConfig((groupId, partition) =>
          this.resolveTrack(
            groupId,
            partition,
            trackDatasetMap,
            sliceUnionDataset,
            slicelikeUnionDataset,
          ),
        ),
      prepareData: async (engine: Engine) => {
        await using trash = new AsyncDisposableStack();

        if (sliceUnionDataset) {
          const query = await this.buildSliceQuery(
            engine,
            sliceUnionDataset,
            area,
            trash,
          );
          unionQueries.push(query);
        }

        if (slicelikeUnionDataset) {
          const query = await this.buildSlicelikeQuery(
            engine,
            slicelikeUnionDataset,
            area,
            trash,
          );
          // Offset group IDs to avoid collision with slice groups
          const groupOffset = sliceTracks.length > 0 ? 1 : 0;
          const offsetQuery = query.replace(
            /__groupid/g,
            `__groupid + ${groupOffset} as __groupid`,
          );
          unionQueries.push(offsetQuery);
        }

        await engine.query(`
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
          SELECT
            json_object('id', id, 'groupid', __groupid, 'partition', __partition) as id_with_lineage,
            name,
            dur,
            self_dur,
            arg_set_id
          FROM (${unionQueries.join(' UNION ALL ')})
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  private async buildSliceQuery(
    engine: Engine,
    unionDataset: UnionDatasetWithLineage<DatasetSchema>,
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<string> {
    // Query with only needed columns for II table (ts, dur, id)
    const iiQuerySchema = {
      ...SLICE_WITH_PARENT_SPEC,
      __groupid: NUM,
      __partition: UNKNOWN,
    };
    const sql = unionDataset.query(iiQuerySchema);

    // Create interval-intersect table for time filtering
    const iiTable = await createIITable(
      engine,
      new SourceDataset({src: `(${sql})`, schema: iiQuerySchema}),
      area.start,
      area.end,
    );
    trash.use(iiTable);

    // Build child duration aggregation for self-time calculation
    const childDurTable = await createPerfettoTable({
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
    trash.use(childDurTable);

    return `
      SELECT
        id,
        name,
        ts,
        dur,
        dur - COALESCE(child_dur, 0) AS self_dur,
        arg_set_id,
        __groupid,
        __partition
      FROM ${iiTable.name}
      LEFT JOIN ${childDurTable.name} USING(id)
    `;
  }

  private async buildSlicelikeQuery(
    engine: Engine,
    unionDataset: UnionDatasetWithLineage<DatasetSchema>,
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<string> {
    // Query with only needed columns for II table (ts, dur, id)
    const iiQuerySchema = {
      ...SLICELIKE_SPEC,
      __groupid: NUM,
      __partition: UNKNOWN,
    };
    const sql = unionDataset.query(iiQuerySchema);

    // Create interval-intersect table for time filtering
    const iiTable = await createIITable(
      engine,
      new SourceDataset({src: `(${sql})`, schema: iiQuerySchema}),
      area.start,
      area.end,
    );
    trash.use(iiTable);

    return `
      SELECT
        id,
        name,
        ts,
        dur,
        dur AS self_dur,
        arg_set_id,
        __groupid,
        __partition
      FROM ${iiTable.name}
    `;
  }

  getTabName() {
    return 'Slices';
  }

  private getGridConfig(
    resolveTrack: (groupId: number, partition: SqlValue) => Track | undefined,
  ): AggregatorGridConfig {
    return {
      schema: {
        id_with_lineage: {
          title: 'ID',
          columnType: 'identifier',
          cellRenderer: (value: unknown) => {
            // Value is a JSON object {id, groupid, partition}
            if (typeof value !== 'string') {
              return String(value);
            }

            const parsed = JSON.parse(value) as {
              id: number;
              groupid: number;
              partition: SqlValue;
            };
            const {id, groupid, partition} = parsed;

            // Resolve track from lineage
            const track = resolveTrack(groupid, partition);
            if (!track) {
              return String(id);
            }

            return m(
              Anchor,
              {
                title: 'Go to slice',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(track.uri, id, {
                    scrollToSelection: true,
                  });
                },
              },
              String(id),
            );
          },
        },
        name: {
          title: 'Name',
          columnType: 'text',
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
        args: {
          title: 'Args',
          parameterized: true,
        },
      },
      // The aggregation table has an `arg_set_id` column, so we can expose a
      // parameterized `args.*` column to the datagrid.
      sqlConfig: ({tableName}): SQLTableSchema => ({
        tableOrSubquery: tableName,
        columns: {
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

  /**
   * Resolve a track from lineage information.
   */
  private resolveTrack(
    groupId: number,
    partition: SqlValue,
    trackDatasetMap: Map<Dataset, Track>,
    sliceUnionDataset?: UnionDatasetWithLineage<DatasetSchema>,
    slicelikeUnionDataset?: UnionDatasetWithLineage<DatasetSchema>,
  ): Track | undefined {
    // Ensure partition is a valid SqlValue
    const partitionValue =
      partition === null ||
      typeof partition === 'number' ||
      typeof partition === 'bigint' ||
      typeof partition === 'string' ||
      partition instanceof Uint8Array
        ? partition
        : null;

    // Try slice union dataset first
    if (sliceUnionDataset) {
      const datasets = sliceUnionDataset.resolveLineage({
        __groupid: groupId,
        __partition: partitionValue,
      });
      for (const dataset of datasets) {
        const track = trackDatasetMap.get(dataset);
        if (track) return track;
      }
    }

    // Try slicelike union dataset (with group offset)
    if (slicelikeUnionDataset) {
      const sliceGroupCount = sliceUnionDataset ? 1 : 0;
      const adjustedGroupId = groupId - sliceGroupCount;
      const datasets = slicelikeUnionDataset.resolveLineage({
        __groupid: adjustedGroupId,
        __partition: partitionValue,
      });
      for (const dataset of datasets) {
        const track = trackDatasetMap.get(dataset);
        if (track) return track;
      }
    }

    return undefined;
  }
}
