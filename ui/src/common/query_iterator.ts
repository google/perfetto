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

import {RawQueryResult} from './protos';

// Union of all the query result formats that we can turn into forward
// iterators.
// TODO(hjd): Replace someOtherEncoding place holder with the real new
// format.
type QueryResult = RawQueryResult|{someOtherEncoding: string};

// One row extracted from an SQL result:
interface Row {
  [key: string]: string|number|null;
}

// API:
// const result = await engine.query("select 42 as n;");
// const it = getRowIterator({"answer": NUM}, result);
// for (; it.valid(); it.next()) {
//   console.log(it.row.answer);
// }
export interface RowIterator<T extends Row> {
  valid(): boolean;
  next(): void;
  row: T;
}

export const NUM = 0;
export const STR = 'str';
export const NUM_NULL: number|null = 1;
export const STR_NULL: string|null = 'str_null';
export type ColumnType =
    (typeof NUM)|(typeof STR)|(typeof NUM_NULL)|(typeof STR_NULL);

// Exported for testing
export function findColumnIndex(
    result: RawQueryResult, name: string, columnType: number|null|string):
    number {
  let matchingDescriptorIndex = -1;
  const disallowNulls = columnType === STR || columnType === NUM;
  const expectsStrings = columnType === STR || columnType === STR_NULL;
  const expectsNumbers = columnType === NUM || columnType === NUM_NULL;
  const isEmpty = +result.numRecords === 0;

  for (let i = 0; i < result.columnDescriptors.length; ++i) {
    const descriptor = result.columnDescriptors[i];
    const column = result.columns[i];
    if (descriptor.name !== name) {
      continue;
    }

    const hasDoubles = column.doubleValues && column.doubleValues.length;
    const hasLongs = column.longValues && column.longValues.length;
    const hasStrings = column.stringValues && column.stringValues.length;

    if (matchingDescriptorIndex !== -1) {
      throw new Error(`Multiple columns with the name ${name}`);
    }

    if (expectsStrings && !hasStrings && !isEmpty) {
      throw new Error(`Expected strings for column ${name} but found numbers`);
    }

    if (expectsNumbers && !hasDoubles && !hasLongs && !isEmpty) {
      throw new Error(`Expected numbers for column ${name} but found strings`);
    }

    if (disallowNulls) {
      for (let j = 0; j < +result.numRecords; ++j) {
        if (column.isNulls![j] === true) {
          throw new Error(`Column ${name} contains nulls`);
        }
      }
    }
    matchingDescriptorIndex = i;
  }

  if (matchingDescriptorIndex === -1) {
    throw new Error(`No column with name ${name} found in result.`);
  }

  return matchingDescriptorIndex;
}

class ColumnarRowIterator {
  row: Row;
  private i_: number;
  private rowCount_: number;
  private columnCount_: number;
  private columnNames_: string[];
  private columns_: Array<number[]|string[]>;
  private nullColumns_: boolean[][];

  constructor(querySpec: Row, queryResult: RawQueryResult) {
    const row: Row = querySpec;
    this.row = row;
    this.i_ = 0;
    this.rowCount_ = +queryResult.numRecords;
    this.columnCount_ = 0;
    this.columnNames_ = [];
    this.columns_ = [];
    this.nullColumns_ = [];

    for (const [columnName, columnType] of Object.entries(querySpec)) {
      const index = findColumnIndex(queryResult, columnName, columnType);
      const column = queryResult.columns[index];
      this.columnCount_++;
      this.columnNames_.push(columnName);
      let values: string[]|Array<number|Long> = [];
      if (column.longValues && column.longValues.length > 0) {
        values = column.longValues;
      }
      if (column.doubleValues && column.doubleValues.length > 0) {
        values = column.doubleValues;
      }
      if (column.stringValues && column.stringValues.length > 0) {
        values = column.stringValues;
      }
      this.columns_.push(values as string[]);
      this.nullColumns_.push(column.isNulls!);
    }
    if (this.rowCount_ > 0) {
      for (let j = 0; j < this.columnCount_; ++j) {
        const name = this.columnNames_[j];
        const isNull = this.nullColumns_[j][this.i_];
        this.row[name] = isNull ? null : this.columns_[j][this.i_];
      }
    }
  }

  valid(): boolean {
    return this.i_ < this.rowCount_;
  }

  next(): void {
    this.i_++;
    for (let j = 0; j < this.columnCount_; ++j) {
      const name = this.columnNames_[j];
      const isNull = this.nullColumns_[j][this.i_];
      this.row[name] = isNull ? null : this.columns_[j][this.i_];
    }
  }
}

// Deliberately not exported, use iter() below to make code easy to switch
// to other queryResult formats.
function iterFromColumns<T extends Row>(
    querySpec: T, queryResult: RawQueryResult): RowIterator<T> {
  const iter = new ColumnarRowIterator(querySpec, queryResult);
  return iter as unknown as RowIterator<T>;
}


function isColumnarQueryResult(result: QueryResult): result is RawQueryResult {
  return (result as RawQueryResult).columnDescriptors !== undefined;
}

export function iter<T extends Row>(
    spec: T, result: QueryResult): RowIterator<T> {
  if (isColumnarQueryResult(result)) {
    return iterFromColumns(spec, result);
  } else {
    throw new Error('Unsuported format');
  }
}

export function slowlyCountRows(result: QueryResult): number {
  if (isColumnarQueryResult(result)) {
    // This isn't actually slow for columnar data but it might be for other
    // formats.
    return +result.numRecords;
  } else {
    throw new Error('Unsuported format');
  }
}
