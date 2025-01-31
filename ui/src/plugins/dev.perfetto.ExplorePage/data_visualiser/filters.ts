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

import {sqliteString} from '../../../base/string_utils';
import {
  LegacySqlTableFilterLabel,
  LegacySqlTableFilterOp,
  LegacySqlTableFilterOptions,
} from '../../../components/widgets/sql/legacy_table/render_cell_utils';
import {ColumnType} from '../../../trace_processor/query_result';

export interface VisFilter {
  filterOption: VisFilterOp;
  columnName: string;
  value?: string;
}

export interface VisFilterOp extends Omit<LegacySqlTableFilterOp, 'label'> {
  label: LegacySqlTableFilterLabel | 'in' | 'between';
}

export const VisFilterOptions: Record<string, VisFilterOp> = {
  ...LegacySqlTableFilterOptions,
  between: {op: 'IN', label: 'in'},
  in: {op: 'BETWEEN', label: 'between'},
};

export function opToVisFilterOption(filterOp: string) {
  return Object.values(VisFilterOptions).filter(({op}) => op === filterOp)[0];
}

export function buildFilterSqlClause(filters: VisFilter[]) {
  return filters.map((filter) => `${filterToSql(filter)}`).join(' AND ');
}

export function filterToSql(filter: VisFilter) {
  const {filterOption, columnName, value} = filter;

  let filterValue: ColumnType | undefined;
  if (value !== undefined) {
    if (Array.isArray(value)) {
      if (filterOption.op === 'in') {
        filterValue = '(';
        value.forEach(
          (v, i) => (filterValue += `${v} ${i < value.length - 1 ? ',' : ''}`),
        );
        filterValue += ')';
      }

      if (filterOption.op === 'between') {
        filterValue = `${value[0]} AND ${value[1]}`;
      }
    } else if (Number.isNaN(Number.parseFloat(value))) {
      filterValue = sqliteString(value);
    } else if (!Number.isInteger(Number.parseFloat(value))) {
      filterValue = Number(value);
    } else {
      filterValue = BigInt(value);
    }
  }

  return `${columnName} ${filterOption.op} ${filterValue === undefined ? '' : filterValue}`;
}
