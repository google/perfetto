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

import {assertTrue} from '../base/logging';

import {RawQueryResult} from './protos';

// These types are used both for the new streaming query iterator and the old
// columnar RawQueryResult.

export const NUM = 0;
export const STR = 'str';
export const NUM_NULL: number|null = 1;
export const STR_NULL: string|null = 'str_null';

export type ColumnType = string|number|null;

// One row extracted from an SQL result:
export interface Row {
  [key: string]: ColumnType;
}

// The methods that any iterator has to implement.
export interface RowIteratorBase {
  valid(): boolean;
  next(): void;
}

// A RowIterator is a type that has all the fields defined in the query spec
// plus the valid() and next() operators. This is to ultimately allow the
// clients to do:
// const result = await engine.queryV2("select name, surname, id from people;");
// const iter = queryResult.iter({name: STR, surname: STR, id: NUM});
// for (; iter.valid(); iter.next())
//  console.log(iter.name, iter.surname);
export type RowIterator<T extends Row> = RowIteratorBase&T;

// The old iterator for non-batched queries. Going away. Usage.
//   const result = await engine.query("select 42 as n;");
//   const it = getRowIterator({"answer": NUM}, result);
//   for (; it.valid(); it.next()) {
//     console.log(it.row.answer);
//   }
export interface LegacyRowIterator<T extends Row> {
  valid(): boolean;
  next(): void;
  row: T;
}

export function columnTypeToString(t: ColumnType): string {
  switch (t) {
    case NUM:
      return 'NUM';
    case NUM_NULL:
      return 'NUM_NULL';
    case STR:
      return 'STR';
    case STR_NULL:
      return 'STR_NULL';
    default:
      return `INVALID(${t})`;
  }
}

// TODO(primiano): the types and helpers in the rest of this file are
// transitional and will be removed once we migrate everything to the streaming
// query API.

// Exported for testing
export function findColumnIndex(
    result: RawQueryResult, name: string, columnType: ColumnType): number {
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
      const isNum = columnType === NUM || columnType === NUM_NULL;
      const isString = columnType === STR || columnType === STR_NULL;
      if (isNum && column.longValues &&
          column.longValues.length === this.rowCount_) {
        values = column.longValues;
      }
      if (isNum && column.doubleValues &&
          column.doubleValues.length === this.rowCount_) {
        values = column.doubleValues;
      }
      if (isString && column.stringValues &&
          column.stringValues.length === this.rowCount_) {
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
    querySpec: T, queryResult: RawQueryResult): LegacyRowIterator<T> {
  const iter = new ColumnarRowIterator(querySpec, queryResult);
  return iter as unknown as LegacyRowIterator<T>;
}

// Deliberately not exported, use iterUntyped() below to make code easy to
// switch to other queryResult formats.
function iterUntypedFromColumns(result: RawQueryResult):
    LegacyRowIterator<Row> {
  const spec: Row = {};
  const desc = result.columnDescriptors;
  for (let i = 0; i < desc.length; ++i) {
    const name = desc[i].name;
    if (!name) {
      continue;
    }
    spec[name] = desc[i].type === 3 ? STR_NULL : NUM_NULL;
  }
  const iter = new ColumnarRowIterator(spec, result);
  return iter as unknown as LegacyRowIterator<Row>;
}

export function iterUntyped(result: RawQueryResult): LegacyRowIterator<Row> {
  return iterUntypedFromColumns(result);
}

export function iter<T extends Row>(
    spec: T, result: RawQueryResult): LegacyRowIterator<T> {
  return iterFromColumns(spec, result);
}

export function slowlyCountRows(result: RawQueryResult): number {
  // This isn't actually slow for columnar data but it might be for other
  // formats.
  return +result.numRecords;
}

export function singleRow<T extends Row>(spec: T, result: RawQueryResult): T|
    undefined {
  const numRows = slowlyCountRows(result);
  if (numRows === 0) {
    return undefined;
  }
  if (numRows > 1) {
    throw new Error(
        `Attempted to extract single row but more than ${numRows} rows found.`);
  }
  const it = iter(spec, result);
  assertTrue(it.valid());
  return it.row;
}

export function singleRowUntyped(result: RawQueryResult): Row|undefined {
  const numRows = slowlyCountRows(result);
  if (numRows === 0) {
    return undefined;
  }
  if (numRows > 1) {
    throw new Error(
        `Attempted to extract single row but more than ${numRows} rows found.`);
  }
  const it = iterUntyped(result);
  assertTrue(it.valid());
  return it.row;
}
