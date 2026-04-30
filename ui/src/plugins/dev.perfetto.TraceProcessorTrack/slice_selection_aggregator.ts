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
  UnionDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import {openDistributionTab} from '../../components/distribution_panel';
import {sliceDistributionCellRenderers} from '../../components/details/slice_details';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  SqlValue,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';
import {MenuItem} from '../../widgets/menu';

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
  // Time-bounded union of all slice-like datasets in the current area
  // selection. Used by the per-row "Histogram" action to open a
  // DistributionPanel scoped to the same events the aggregator is summarizing.
  private distributionDataset?: Dataset;

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
      this.distributionDataset = undefined;
      return undefined;
    }

    // Build the dataset that backs the per-row "Histogram" action. Mirrors
    // how the area-selection flamegraph builds its distribution dataset:
    // union of every slice-like track in the selection, wrapped with a
    // time-window filter so the resulting histogram reflects the same
    // events the user is looking at in the aggregator.
    const trackDatasets: Dataset[] = [];
    for (const track of [...sliceTracks, ...slicelikeTracks]) {
      const ds = track.renderer.getDataset?.();
      if (ds !== undefined) trackDatasets.push(ds);
    }
    if (trackDatasets.length > 0) {
      const combined =
        trackDatasets.length === 1
          ? trackDatasets[0]
          : UnionDataset.create(trackDatasets);
      this.distributionDataset = new SourceDataset({
        src: `
          select * from (${combined.query(SLICELIKE_SPEC)})
          where ts < ${area.end}
            and ts + dur > ${area.start}
        `,
        schema: SLICELIKE_SPEC,
      });
    } else {
      this.distributionDataset = undefined;
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
            json_object('id', id, 'groupid', __groupid, 'partition', __partition) as id_with_lineage,
            name,
            dur,
            self_dur
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
      groupBy: [{id: 'name', field: 'name'}],
      aggregates: [
        {id: 'count', function: 'COUNT'},
        {id: 'total_time_sum', field: 'dur', function: 'SUM', sort: 'DESC'},
        {id: 'self_time_sum', field: 'self_dur', function: 'SUM'},
        {id: 'total_time_avg', field: 'dur', function: 'AVG'},
      ],
      columns: [
        {
          title: 'ID',
          columnId: 'id_with_lineage',
          formatHint: 'ID',
          cellRenderer: (value: unknown) => {
            // Value is a JSON object {id, groupid, partition}
            if (typeof value !== 'string') {
              return String(value);
            }

            const parsed = JSON.parse(value) as {
              id: number;
              groupid: number;
              partition: unknown;
            };
            const {id, groupid, partition} = parsed;

            // Resolve track from lineage
            const track = this.resolveTrack(groupid, partition);
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

  // Per-row "Histogram" menu item appended to the row popup menu (alongside
  // the built-in "Drill down" item). Opens a DistributionPanel scoped to the
  // area selection, mirroring how the area-selection flamegraph's per-node
  // "Find matching slices" action works.
  extraPivotRowMenuItems(
    drillDown: ReadonlyArray<{readonly field: string; readonly value: SqlValue}>,
  ): m.Children {
    const dataset = this.distributionDataset;
    if (dataset === undefined) return undefined;
    const nameEntry = drillDown.find((d) => d.field === 'name');
    if (nameEntry === undefined || typeof nameEntry.value !== 'string') {
      return undefined;
    }
    const name = nameEntry.value;
    return m(MenuItem, {
      label: 'Show duration histogram',
      icon: 'bar_chart',
      onclick: () =>
        openDistributionTab(this.trace, {
          title: `${name} (in selection)`,
          dataset,
          filter: {col: 'name', eq: name},
          valueColumn: 'dur',
          idColumn: 'id',
          sqlTable: 'slice',
          displayColumns: ['ts', 'dur'],
          cellRenderers: sliceDistributionCellRenderers(this.trace),
        }),
    });
  }
}
