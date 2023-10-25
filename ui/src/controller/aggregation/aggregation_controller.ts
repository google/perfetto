// Copyright (C) 2019 The Android Open Source Project
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

import {isString} from '../../base/object_utils';
import {
  AggregateData,
  Column,
  ColumnDef,
  ThreadStateExtra,
} from '../../common/aggregation_data';
import {Engine} from '../../common/engine';
import {NUM} from '../../common/query_result';
import {Area, Sorting} from '../../common/state';
import {globals} from '../../frontend/globals';
import {publishAggregateData} from '../../frontend/publish';
import {AreaSelectionHandler} from '../area_selection_handler';
import {Controller} from '../controller';

export interface AggregationControllerArgs {
  engine: Engine;
  kind: string;
}

function isStringColumn(column: Column): boolean {
  return column.kind === 'STRING' || column.kind === 'STATE';
}

export abstract class AggregationController extends Controller<'main'> {
  readonly kind: string;
  private areaSelectionHandler: AreaSelectionHandler;
  private previousSorting?: Sorting;
  private requestingData = false;
  private queuedRequest = false;

  abstract createAggregateView(engine: Engine, area: Area): Promise<boolean>;

  abstract getExtra(engine: Engine, area: Area): Promise<ThreadStateExtra|void>;

  abstract getTabName(): string;
  abstract getDefaultSorting(): Sorting;
  abstract getColumnDefinitions(): ColumnDef[];

  constructor(private args: AggregationControllerArgs) {
    super('main');
    this.kind = this.args.kind;
    this.areaSelectionHandler = new AreaSelectionHandler();
  }

  run() {
    const selection = globals.state.currentSelection;
    if (selection === null || selection.kind !== 'AREA') {
      publishAggregateData({
        data: {
          tabName: this.getTabName(),
          columns: [],
          strings: [],
          columnSums: [],
        },
        kind: this.args.kind,
      });
      return;
    }
    const aggregatePreferences =
        globals.state.aggregatePreferences[this.args.kind];

    const sortingChanged = aggregatePreferences &&
        this.previousSorting !== aggregatePreferences.sorting;
    const [hasAreaChanged, area] = this.areaSelectionHandler.getAreaChange();
    if ((!hasAreaChanged && !sortingChanged) || !area) return;

    if (this.requestingData) {
      this.queuedRequest = true;
    } else {
      this.requestingData = true;
      if (sortingChanged) this.previousSorting = aggregatePreferences.sorting;
      this.getAggregateData(area, hasAreaChanged)
          .then((data) => publishAggregateData({data, kind: this.args.kind}))
          .finally(() => {
            this.requestingData = false;
            if (this.queuedRequest) {
              this.queuedRequest = false;
              this.run();
            }
          });
    }
  }

  async getAggregateData(area: Area, areaChanged: boolean):
      Promise<AggregateData> {
    if (areaChanged) {
      const viewExists = await this.createAggregateView(this.args.engine, area);
      if (!viewExists) {
        return {
          tabName: this.getTabName(),
          columns: [],
          strings: [],
          columnSums: [],
        };
      }
    }

    const defs = this.getColumnDefinitions();
    const colIds = defs.map((col) => col.columnId);
    const pref = globals.state.aggregatePreferences[this.kind];
    let sorting = `${this.getDefaultSorting().column} ${
        this.getDefaultSorting().direction}`;
    if (pref && pref.sorting) {
      sorting = `${pref.sorting.column} ${pref.sorting.direction}`;
    }
    const query = `select ${colIds} from ${this.kind} order by ${sorting}`;
    const result = await this.args.engine.query(query);

    const numRows = result.numRows();
    const columns = defs.map((def) => this.columnFromColumnDef(def, numRows));
    const columnSums = await Promise.all(defs.map((def) => this.getSum(def)));
    const extraData = await this.getExtra(this.args.engine, area);
    const extra = extraData ? extraData : undefined;
    const data: AggregateData =
        {tabName: this.getTabName(), columns, columnSums, strings: [], extra};

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
          // panels in it's current form is not worth it. Thus, we simply
          // convert bigints to numbers.
          column.data[i] = Number(item);
        } else {
          column.data[i] = item;
        }
      }
    }

    return data;
  }

  async getSum(def: ColumnDef): Promise<string> {
    if (!def.sum) return '';
    const result = await this.args.engine.query(
        `select ifnull(sum(${def.columnId}), 0) as s from ${this.kind}`);
    let sum = result.firstRow({s: NUM}).s;
    if (def.kind === 'TIMESTAMP_NS') {
      sum = sum / 1e6;
    }
    return `${sum}`;
  }

  columnFromColumnDef(def: ColumnDef, numRows: number): Column {
    // TODO(hjd): The Column type should be based on the
    // ColumnDef type or vice versa to avoid this cast.
    return {
      title: def.title,
      kind: def.kind,
      data: new def.columnConstructor(numRows),
      columnId: def.columnId,
    } as Column;
  }
}
