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
  type AggregationData,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
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

interface SliceAggregationData extends AggregationData {
  readonly trackDatasetMap: ReadonlyMap<Dataset, Track>;
  readonly sliceUnionDataset:
    UnionDatasetWithLineage<DatasetSchema> | undefined;
  readonly slicelikeUnionDataset:
    UnionDatasetWithLineage<DatasetSchema> | undefined;
}

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

    return {
      prepareData: async (engine: Engine) => {
        const unionQueries: string[] = [];
        await using trash = new AsyncDisposableStack();
        const trackDatasetMap = new Map<Dataset, Track>();
        let sliceUnionDataset:
          UnionDatasetWithLineage<DatasetSchema> | undefined;
        let slicelikeUnionDataset:
          UnionDatasetWithLineage<DatasetSchema> | undefined;

        if (sliceTracks.length > 0) {
          const {
            query,
            unionDataset,
            trackDatasetMap: sliceTrackDatasetMap,
          } = await this.buildSliceQuery(engine, sliceTracks, area, trash);
          unionQueries.push(query);
          sliceUnionDataset = unionDataset;
          for (const [dataset, track] of sliceTrackDatasetMap.entries()) {
            trackDatasetMap.set(dataset, track);
          }
        }

        if (slicelikeTracks.length > 0) {
          const {
            query,
            unionDataset,
            trackDatasetMap: slicelikeTrackDatasetMap,
          } = await this.buildSlicelikeQuery(
            engine,
            slicelikeTracks,
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
          slicelikeUnionDataset = unionDataset;
          for (const [dataset, track] of slicelikeTrackDatasetMap.entries()) {
            trackDatasetMap.set(dataset, track);
          }
        }

        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              json_object('id', id, 'groupid', __groupid, 'partition', __partition) as id_with_lineage,
              ts,
              name,
              dur,
              self_dur,
              arg_set_id
            FROM (${unionQueries.join(' UNION ALL ')})
        `,
        });

        return {
          ...createAggregationData(table),
          trackDatasetMap,
          sliceUnionDataset,
          slicelikeUnionDataset,
        };
      },
    };
  }

  private async buildSliceQuery(
    engine: Engine,
    tracks: Track[],
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<{
    query: string;
    unionDataset: UnionDatasetWithLineage<DatasetSchema>;
    trackDatasetMap: Map<Dataset, Track>;
  }> {
    // Build track-to-dataset mapping
    const trackDatasetMap = new Map<Dataset, Track>();
    const datasets: Dataset[] = [];
    for (const track of tracks) {
      const dataset = track.renderer.getDataset?.();
      if (dataset) {
        datasets.push(dataset);
        trackDatasetMap.set(dataset, track);
      }
    }

    // Create union dataset with lineage tracking
    const unionDataset = UnionDatasetWithLineage.create(datasets);

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

    return {
      query: `
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
      `,
      unionDataset,
      trackDatasetMap,
    };
  }

  private async buildSlicelikeQuery(
    engine: Engine,
    tracks: Track[],
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<{
    query: string;
    unionDataset: UnionDatasetWithLineage<DatasetSchema>;
    trackDatasetMap: Map<Dataset, Track>;
  }> {
    // Build track-to-dataset mapping
    const trackDatasetMap = new Map<Dataset, Track>();
    const datasets: Dataset[] = [];
    for (const track of tracks) {
      const dataset = track.renderer.getDataset?.();
      if (dataset) {
        datasets.push(dataset);
        trackDatasetMap.set(dataset, track);
      }
    }

    // Create union dataset with lineage tracking
    const unionDataset = UnionDatasetWithLineage.create(datasets);

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

    return {
      query: `
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
      `,
      unionDataset,
      trackDatasetMap,
    };
  }

  getTabName() {
    return 'Slices';
  }

  getGridConfig(data?: AggregationData): AggregatorGridConfig {
    const sliceData = data as SliceAggregationData | undefined;
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
            const track =
              sliceData && this.resolveTrack(sliceData, groupid, partition);
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
                    switchToCurrentSelectionTab: false,
                  });
                },
              },
              String(id),
            );
          },
        },
        ts: {
          title: 'Timestamp',
          columnType: 'quantitative',
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
        arg_set_id: {
          title: 'Arg set ID',
          columnType: 'identifier',
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

  /**
   * Resolve a track from lineage information.
   */
  private resolveTrack(
    data: SliceAggregationData,
    groupId: number,
    partition: SqlValue,
  ): Track | undefined {
    const {trackDatasetMap, sliceUnionDataset, slicelikeUnionDataset} = data;

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
