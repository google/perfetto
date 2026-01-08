// Copyright (C) 2020 The Android Open Source Project
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
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {Icons} from '../../base/semantic_icons';
import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Track} from '../../public/track';
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  Row,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';

const SLICE_WITH_PARENT_SPEC = {
  id: NUM,
  name: STR_NULL,
  ts: LONG,
  dur: LONG,
  parent_id: NUM_NULL,
};

const SLICELIKE_SPEC = {
  id: NUM,
  name: STR_NULL,
  ts: LONG,
  dur: LONG,
};

export class SliceSelectionAggregator implements Aggregator {
  readonly id = 'slice_aggregation';

  private readonly trace: Trace;
  // Store track-to-dataset mapping for lineage resolution
  private trackDatasetMap?: Map<Dataset, Track>;
  // Store union datasets for lineage resolution
  private sliceUnionDataset?: UnionDatasetWithLineage<DatasetSchema>;
  private slicelikeUnionDataset?: UnionDatasetWithLineage<DatasetSchema>;

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
        this.trackDatasetMap = new Map();

        if (sliceTracks.length > 0) {
          const {query, unionDataset, trackDatasetMap} =
            await this.buildSliceQuery(engine, sliceTracks, area, trash);
          unionQueries.push(query);
          this.sliceUnionDataset = unionDataset;
          for (const [dataset, track] of trackDatasetMap.entries()) {
            this.trackDatasetMap.set(dataset, track);
          }
        }

        if (slicelikeTracks.length > 0) {
          const {query, unionDataset, trackDatasetMap} =
            await this.buildSlicelikeQuery(
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
          this.slicelikeUnionDataset = unionDataset;
          for (const [dataset, track] of trackDatasetMap.entries()) {
            this.trackDatasetMap.set(dataset, track);
          }
        }

        await engine.query(`
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
          SELECT
            id,
            name,
            dur,
            self_dur,
            __groupid,
            __partition
          FROM (${unionQueries.join(' UNION ALL ')})
        `);

        return {tableName: this.id};
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

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [{field: 'name'}],
      aggregates: [
        {function: 'COUNT'},
        {field: 'dur', function: 'SUM', sort: 'DESC'},
        {field: 'self_dur', function: 'SUM'},
        {field: 'dur', function: 'AVG'},
      ],
      columns: [
        {
          title: 'ID',
          columnId: 'id',
          formatHint: 'ID',
          dependsOn: ['__groupid', '__partition'],
          cellRenderer: (value: unknown, row: Row) => {
            if (typeof value !== 'bigint') {
              return String(value);
            }

            const groupId = row['__groupid'];
            const partition = row['__partition'];

            if (typeof groupId !== 'bigint') {
              return String(value);
            }

            // Resolve track from lineage
            const track = this.resolveTrack(Number(groupId), partition);
            if (!track) {
              return String(value);
            }

            return m(
              Anchor,
              {
                title: 'Go to slice',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(
                    track.uri,
                    Number(value),
                    {
                      scrollToSelection: true,
                    },
                  );
                },
              },
              String(value),
            );
          },
        },
        {
          title: 'Name',
          columnId: 'name',
          formatHint: 'STRING',
        },
        {
          title: 'Wall Duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
        {
          title: 'Self Duration',
          formatHint: 'DURATION_NS',
          columnId: 'self_dur',
        },
      ],
    };
  }

  /**
   * Resolve a track from lineage information.
   */
  private resolveTrack(groupId: number, partition: unknown): Track | undefined {
    if (!this.trackDatasetMap) return undefined;

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
    if (this.sliceUnionDataset) {
      const datasets = this.sliceUnionDataset.resolveLineage({
        __groupid: groupId,
        __partition: partitionValue,
      });
      for (const dataset of datasets) {
        const track = this.trackDatasetMap.get(dataset);
        if (track) return track;
      }
    }

    // Try slicelike union dataset (with group offset)
    if (this.slicelikeUnionDataset) {
      const sliceGroupCount = this.sliceUnionDataset ? 1 : 0;
      const adjustedGroupId = groupId - sliceGroupCount;
      const datasets = this.slicelikeUnionDataset.resolveLineage({
        __groupid: adjustedGroupId,
        __partition: partitionValue,
      });
      for (const dataset of datasets) {
        const track = this.trackDatasetMap.get(dataset);
        if (track) return track;
      }
    }

    return undefined;
  }
}
