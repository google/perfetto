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
import {SourceDataset} from '../../trace_processor/dataset';
import {
  buildQueryWithLineage,
  LineageResolver,
} from '../../trace_processor/dataset_query_utils';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  Row,
  STR_NULL,
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
  // Store lineage resolver for use when rendering clickable IDs
  private lineageResolver?: LineageResolver<Track>;

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
        const lineageResolvers: LineageResolver<Track>[] = [];
        await using trash = new AsyncDisposableStack();

        if (sliceTracks.length > 0) {
          const {query, resolver} = await this.buildSliceQuery(
            engine,
            sliceTracks,
            area,
            trash,
          );
          unionQueries.push(query);
          lineageResolvers.push(resolver);
        }

        if (slicelikeTracks.length > 0) {
          const {query, resolver} = await this.buildSlicelikeQuery(
            engine,
            slicelikeTracks,
            area,
            trash,
          );
          // Offset groupids for slicelike resolver
          const sliceGroupCount = sliceTracks.length > 0 ? 1 : 0;
          unionQueries.push(
            query.replace(
              /z__groupid/g,
              `z__groupid + ${sliceGroupCount} AS z__groupid`,
            ),
          );
          lineageResolvers.push(resolver);
        }

        // Combine lineage resolvers
        this.lineageResolver = {
          resolve: (groupId, partition) => {
            // Route to correct resolver based on groupId
            for (const resolver of lineageResolvers) {
              const result = resolver.resolve(groupId, partition);
              if (result) return result;
            }
            return undefined;
          },
          getGroupInputs: (groupId) => {
            for (const resolver of lineageResolvers) {
              const inputs = resolver.getGroupInputs(groupId);
              if (inputs.length > 0) return inputs;
            }
            return [];
          },
        };

        await engine.query(`
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
          SELECT
            id,
            name,
            dur,
            self_dur,
            z__groupid,
            z__partition
          FROM (${unionQueries.join(' UNION ALL ')})
        `);

        return {tableName: this.id, lineageResolver: this.lineageResolver};
      },
    };
  }

  private async buildSliceQuery(
    engine: Engine,
    tracks: Track[],
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<{query: string; resolver: LineageResolver<Track>}> {
    // Build query with lineage tracking
    const {sql, lineageResolver} = buildQueryWithLineage({
      inputs: tracks,
      datasetFetcher: (t) => t.renderer.getDataset?.() as SourceDataset,
      columns: SLICE_WITH_PARENT_SPEC,
    });

    // Schema including lineage columns
    const schemaWithLineage = {
      ...SLICE_WITH_PARENT_SPEC,
      z__groupid: NUM,
      z__partition: NUM,
    };

    // Create interval-intersect table for time filtering
    const iiTable = await createIITable(
      engine,
      new SourceDataset({src: `(${sql})`, schema: schemaWithLineage}),
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
          z__groupid,
          z__partition
        FROM ${iiTable.name}
        LEFT JOIN ${childDurTable.name} USING(id)
      `,
      resolver: lineageResolver,
    };
  }

  private async buildSlicelikeQuery(
    engine: Engine,
    tracks: Track[],
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<{query: string; resolver: LineageResolver<Track>}> {
    // Build query with lineage tracking
    const {sql, lineageResolver} = buildQueryWithLineage({
      inputs: tracks,
      datasetFetcher: (t) => t.renderer.getDataset?.() as SourceDataset,
      columns: SLICELIKE_SPEC,
    });

    // Schema including lineage columns
    const schemaWithLineage = {
      ...SLICELIKE_SPEC,
      z__groupid: NUM,
      z__partition: NUM,
    };

    // Create interval-intersect table for time filtering
    const iiTable = await createIITable(
      engine,
      new SourceDataset({src: `(${sql})`, schema: schemaWithLineage}),
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
          z__groupid,
          z__partition
        FROM ${iiTable.name}
      `,
      resolver: lineageResolver,
    };
  }

  getLineageResolver(): LineageResolver<Track> | undefined {
    return this.lineageResolver;
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
          cellRenderer: (value: unknown, row: Row) => {
            if (typeof value !== 'bigint') {
              return String(value);
            }

            const groupId = row['z__groupid'];
            const partition = row['z__partition'];

            if (
              typeof groupId !== 'bigint' ||
              (typeof partition !== 'bigint' && partition !== null)
            ) {
              return String(value);
            }

            const track = this.lineageResolver?.resolve(
              Number(groupId),
              Number(partition),
            );
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
        {
          title: 'Partition',
          columnId: 'z__partition',
          formatHint: 'ID',
        },
        {
          title: 'GroupID',
          columnId: 'z__groupid',
          formatHint: 'ID',
        },
      ],
    };
  }
}
