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

import {AsyncDisposableStack} from '../../base/disposable_stack';
import {ColumnDef, Sorting} from '../../components/aggregation';
import {
  Aggregation,
  Aggregator,
  createIITable,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Dataset, createUnionDataset} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

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

  probe(area: AreaSelection): Aggregation | undefined {
    const sliceDatasets: Array<Dataset<typeof SLICE_WITH_PARENT_SPEC>> = [];
    const slicelikeDatasets: Array<Dataset<typeof SLICELIKE_SPEC>> = [];

    // Pick tracks we can aggregate, sorting them into slice and slicelike
    // buckets
    for (const track of area.tracks) {
      const dataset = track.renderer.getDataset?.();
      if (!dataset) continue;

      if (dataset.implements(SLICE_WITH_PARENT_SPEC)) {
        sliceDatasets.push(dataset);
      } else if (dataset.implements(SLICELIKE_SPEC)) {
        slicelikeDatasets.push(dataset);
      }
    }

    if (sliceDatasets.length === 0 && slicelikeDatasets.length === 0) {
      return undefined;
    }

    return {
      prepareData: async (engine: Engine) => {
        const unionQueries: string[] = [];
        await using trash = new AsyncDisposableStack();

        if (sliceDatasets.length > 0) {
          const query = await this.buildSliceQuery(
            engine,
            createUnionDataset(sliceDatasets).optimize(),
            area,
            trash,
          );
          unionQueries.push(query);
        }

        if (slicelikeDatasets.length > 0) {
          const query = await this.buildSlicelikeQuery(
            engine,
            createUnionDataset(slicelikeDatasets).optimize(),
            area,
            trash,
          );
          unionQueries.push(query);
        }

        await engine.query(`
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
          SELECT
            name,
            SUM(dur) AS total_dur,
            SUM(dur) / COUNT() AS avg_dur,
            COUNT() AS occurrences,
            SUM(self_dur) AS total_self_dur
          FROM (${unionQueries.join(' UNION ALL ')})
          GROUP BY name
        `);

        return {tableName: this.id};
      },
    };
  }

  private async buildSliceQuery(
    engine: Engine,
    sliceTracks: Dataset<typeof SLICE_WITH_PARENT_SPEC>,
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<string> {
    const iiTable = await createIITable(
      engine,
      sliceTracks,
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
        dur - COALESCE(child_dur, 0) AS self_dur
      FROM ${iiTable.name}
      LEFT JOIN ${childDurTable.name} USING(id)
    `;
  }

  private async buildSlicelikeQuery(
    engine: Engine,
    slicelikeTracks: Dataset<typeof SLICELIKE_SPEC>,
    area: AreaSelection,
    trash: AsyncDisposableStack,
  ): Promise<string> {
    const iiTable = await createIITable(
      engine,
      slicelikeTracks,
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
        dur AS self_dur
      FROM ${iiTable.name}
    `;
  }

  getTabName() {
    return 'Slices';
  }

  getDefaultSorting(): Sorting {
    return {column: 'total_dur', direction: 'DESC'};
  }

  getColumnDefinitions(): ColumnDef[] {
    return [
      {
        title: 'Name',
        columnId: 'name',
      },
      {
        title: 'Wall duration',
        formatHint: 'DURATION_NS',
        columnId: 'total_dur',
        sum: true,
      },
      {
        title: 'Self duration',
        formatHint: 'DURATION_NS',
        columnId: 'total_self_dur',
        sum: true,
      },
      {
        title: 'Avg Wall duration',
        formatHint: 'DURATION_NS',
        columnId: 'avg_dur',
      },
      {
        title: 'Occurrences',
        columnId: 'occurrences',
        sum: true,
      },
    ];
  }
}
