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

import {ColumnDef, Sorting} from '../../components/aggregation';
import {
  Aggregation,
  Aggregator,
  createIITable,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

export class SliceSelectionAggregator implements Aggregator {
  readonly id = 'slice_aggregation';

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(area.tracks, {
      id: NUM,
      name: STR_NULL,
      ts: LONG,
      dur: LONG,
      parent_id: NUM_NULL,
    });

    if (!dataset) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        await using iiTable = await createIITable(
          engine,
          dataset,
          area.start,
          area.end,
        );

        // Build a table containing the sums of all child slices for each parent
        // slice. We can subtract this later from the parent's wall duration to
        // calculate the parent's self time.
        await using childSliceSelfTime = await createPerfettoTable({
          engine,
          as: `
            SELECT
              parent_id AS id,
              SUM(dur) AS child_dur
            FROM (${iiTable.name})
            WHERE parent_id IS NOT NULL
            GROUP BY parent_id
          `,
        });

        await engine.query(`
          create or replace perfetto table ${this.id} as
          select
            name,
            sum(dur) AS total_dur,
            sum(dur)/count() as avg_dur,
            count() as occurrences,
            SUM(dur - COALESCE(child_dur, 0)) AS total_self_dur
          from (${iiTable.name})
          LEFT JOIN ${childSliceSelfTime.name} USING(id)
          group by name
        `);

        return {
          tableName: this.id,
        };
      },
    };
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
