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

export type Column = (
  | StringColumn
  | TimestampColumn
  | NumberColumn
  | StateColumn
) & {
  readonly title: string;
  readonly columnId: string;
};

export interface StringColumn {
  readonly kind: 'STRING';
  readonly data: Uint16Array;
}

export interface TimestampColumn {
  readonly kind: 'TIMESTAMP_NS';
  readonly data: Float64Array;
}

export interface NumberColumn {
  readonly kind: 'NUMBER';
  readonly data: Uint16Array;
}

export interface StateColumn {
  readonly kind: 'STATE';
  readonly data: Uint16Array;
}

type TypedArrayConstructor =
  | Uint16ArrayConstructor
  | Float64ArrayConstructor
  | Uint32ArrayConstructor;
export interface ColumnDef {
  readonly title: string;
  readonly kind: string;
  readonly sum?: boolean;
  readonly columnConstructor: TypedArrayConstructor;
  readonly columnId: string;
}

export interface AggregateData {
  readonly tabName: string;
  readonly columns: Column[];
  readonly columnSums: string[];
  // For string interning.
  readonly strings: string[];
  // Some aggregations will have extra info to display;
  readonly extra?: ThreadStateExtra;
}

export function isEmptyData(data: AggregateData) {
  return data.columns.length === 0 || data.columns[0].data.length === 0;
}

export interface ThreadStateExtra {
  readonly kind: 'THREAD_STATE';
  readonly states: string[];
  readonly values: Float64Array;
  readonly totalMs: number;
}

export interface Sorting {
  readonly column: string;
  readonly direction: 'DESC' | 'ASC';
}
