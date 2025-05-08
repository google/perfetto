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

import {AsyncLimiter} from '../base/async_limiter';
import {isString} from '../base/object_utils';
import {AggregateData, Column, ColumnDef, Sorting} from '../public/aggregation';
import {AreaSelection, AreaSelectionAggregator} from '../public/selection';
import {Track} from '../public/track';
import {Dataset, UnionDataset} from '../trace_processor/dataset';
import {Engine} from '../trace_processor/engine';
import {NUM} from '../trace_processor/query_result';

export class SelectionAggregationManager {
  private readonly limiter = new AsyncLimiter();
  private _sorting?: Sorting;
  private _currentArea: AreaSelection | undefined = undefined;
  private _aggregatedData?: AggregateData;

  constructor(
    private readonly engine: Engine,
    private readonly aggregator: AreaSelectionAggregator,
  ) {}

  get aggregatedData(): AggregateData | undefined {
    return this._aggregatedData;
  }

  aggregateArea(area: AreaSelection) {
    this.limiter.schedule(async () => {
      this._currentArea = area;
      this._aggregatedData = undefined;

      const data = await this.runAggregator(area);
      this._aggregatedData = data;
    });
  }

  clear() {
    // This is wrapped in the async limiter to make sure that an aggregateArea()
    // followed by a clear() (e.g., because selection changes) doesn't end up
    // with the aggregation being displayed anyways once the promise completes.
    this.limiter.schedule(async () => {
      this._currentArea = undefined;
      this._aggregatedData = undefined;
      this._sorting = undefined;
    });
  }

  getSortingPrefs(): Sorting | undefined {
    return this._sorting;
  }

  toggleSortingColumn(column: string) {
    const sorting = this._sorting;
    if (sorting === undefined || sorting.column !== column) {
      // No sorting set for current column.
      this._sorting = {
        column,
        direction: 'DESC',
      };
    } else if (sorting.direction === 'DESC') {
      // Toggle the direction if the column is currently sorted.
      this._sorting = {
        column,
        direction: 'ASC',
      };
    } else {
      // If direction is currently 'ASC' toggle to no sorting.
      this._sorting = undefined;
    }

    // Re-run the aggregation.
    if (this._currentArea) {
      this.aggregateArea(this._currentArea);
    }
  }

  private async runAggregator(
    area: AreaSelection,
  ): Promise<AggregateData | undefined> {
    const aggr = this.aggregator;
    const dataset = this.createDatasetForAggregator(aggr, area.tracks);
    const viewExists = await aggr.createAggregateView(
      this.engine,
      area,
      dataset,
    );

    if (!viewExists) {
      return undefined;
    }

    const defs = aggr.getColumnDefinitions();
    const colIds = defs.map((col) => col.columnId);
    const sorting = this._sorting;
    let sortClause = `${aggr.getDefaultSorting().column} ${
      aggr.getDefaultSorting().direction
    }`;
    if (sorting) {
      sortClause = `${sorting.column} ${sorting.direction}`;
    }
    const query = `select ${colIds} from ${aggr.id} order by ${sortClause}`;
    const result = await this.engine.query(query);

    const numRows = result.numRows();
    const columns = defs.map((def) => columnFromColumnDef(def, numRows));
    const columnSums = await Promise.all(
      defs.map((def) => this.getSum(aggr.id, def)),
    );
    const extraData = await aggr.getExtra(this.engine, area);
    const extra = extraData ? extraData : undefined;
    const data: AggregateData = {
      tabName: aggr.getTabName(),
      columns,
      columnSums,
      strings: [],
      extra,
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = data.strings.length;
      data.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    const it = result.iter({});
    for (let i = 0; it.valid(); it.next(), ++i) {
      for (const column of data.columns) {
        const item = it.get(column.columnId);
        if (item === null) {
          column.data[i] = isStringColumn(column) ? internString('NULL') : 0;
        } else if (isString(item)) {
          column.data[i] = internString(item);
        } else if (item instanceof Uint8Array) {
          column.data[i] = internString('<Binary blob>');
        } else if (typeof item === 'bigint') {
          // TODO(stevegolton) It would be nice to keep bigints as bigints for
          // the purposes of aggregation, however the aggregation infrastructure
          // is likely to be significantly reworked when we introduce EventSet,
          // and the complexity of supporting bigints throughout the aggregation
          // panels in its current form is not worth it. Thus, we simply
          // convert bigints to numbers.
          column.data[i] = Number(item);
        } else {
          column.data[i] = item;
        }
      }
    }

    return data;
  }

  private createDatasetForAggregator(
    aggr: AreaSelectionAggregator,
    tracks: ReadonlyArray<Track>,
  ): Dataset | undefined {
    const filteredDatasets = tracks
      .filter(
        (td) =>
          aggr.trackKind === undefined || aggr.trackKind === td.tags?.kind,
      )
      .map((td) => td.track.getDataset?.())
      .filter((dataset) => dataset !== undefined)
      .filter(
        (dataset) =>
          aggr.schema === undefined || dataset.implements(aggr.schema),
      );

    if (filteredDatasets.length === 0) return undefined;
    return new UnionDataset(filteredDatasets).optimize();
  }

  private async getSum(tableName: string, def: ColumnDef): Promise<string> {
    if (!def.sum) return '';
    const result = await this.engine.query(
      `select ifnull(sum(${def.columnId}), 0) as s from ${tableName}`,
    );
    let sum = result.firstRow({s: NUM}).s;
    if (def.kind === 'TIMESTAMP_NS') {
      sum = sum / 1e6;
    }
    return `${sum}`;
  }
}

function columnFromColumnDef(def: ColumnDef, numRows: number): Column {
  // TODO(hjd): The Column type should be based on the
  // ColumnDef type or vice versa to avoid this cast.
  return {
    title: def.title,
    kind: def.kind,
    data: new def.columnConstructor(numRows),
    columnId: def.columnId,
  } as Column;
}

function isStringColumn(column: Column): boolean {
  return column.kind === 'STRING' || column.kind === 'STATE';
}
