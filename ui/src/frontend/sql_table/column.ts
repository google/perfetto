// Copyright (C) 2023 The Android Open Source Project
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

import {sqliteString} from '../../base/string_utils';

import {
  ArgSetIdColumn,
  dependendentColumns,
  DisplayConfig,
  RegularSqlTableColumn,
} from './table_description';

// This file contains the defintions of different column types that can be
// displayed in the table viewer.

export interface Column {
  // SQL expression calculating the value of this column.
  expression: string;
  // Unique name for this column.
  // The relevant bit of SQL fetching this column will be ${expression} as
  // ${alias}.
  alias: string;
  // Title to be displayed in the table header.
  title: string;
  // How the value of this column should be rendered.
  display?: DisplayConfig;
}

export function columnFromSqlTableColumn(c: RegularSqlTableColumn): Column {
  return {
    expression: c.name,
    alias: c.name,
    title: c.title || c.name,
    display: c.display,
  };
}

export function argColumn(c: ArgSetIdColumn, argName: string): Column {
  const escape = (name: string) => name.replace(/[^A-Za-z0-9]/g, '_');
  return {
    expression: `extract_arg(${c.name}, ${sqliteString(argName)})`,
    alias: `_arg_${c.name}_${escape(argName)}`,
    title: `${c.title ?? c.name} ${argName}`,
  };
}

// A single instruction from a select part of the SQL statement, i.e.
// select `expression` as `alias`.
export type SqlProjection = {
  expression: string,
  alias: string,
};

export function formatSqlProjection(p: SqlProjection): string {
  return `${p.expression} as ${p.alias}`;
}

// Returns a list of projections (i.e. parts of the SELECT clause) that should
// be added to the query fetching the data to be able to display the given
// column (e.g. `foo` or `f(bar) as baz`).
// Some table columns are backed by multiple SQL columns (e.g. slice_id is
// backed by id, ts, dur and track_id), so we need to return a list.
export function sqlProjectionsForColumn(column: Column): SqlProjection[] {
  const result: SqlProjection[] = [{
    expression: column.expression,
    alias: column.alias,
  }];
  for (const dependency of dependendentColumns(column.display)) {
    result.push({
      expression: dependency,
      alias: dependency,
    });
  }
  return result;
}
