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

import {Item, SignalValue} from 'vega';
import {sqliteString} from '../../../base/string_utils';
import {ColumnType} from '../../../trace_processor/query_result';
import {
  VegaLiteFieldType,
  VegaLiteSelectionTypes,
} from '../../../components/widgets/vega_view';

export interface VisFilter {
  filterOption: VisFilterOption;
  columnName: string;
  value?: string;
}

interface VisFilterOp {
  label: string; // human readable name for operation
  op: string; // string representation of the operation (to be injected to SQL)
}

export enum VisFilterOption {
  GLOB,
  EQUALS_TO,
  NOT_EQUALS_TO,
  GREATER_THAN,
  GREATER_OR_EQUALS_THAN,
  LESS_THAN,
  LESS_OR_EQUALS_THAN,
  IS_NULL,
  IS_NOT_NULL,
  IN,
  BETWEEN,
}

export const VIS_FILTER_OPTION_TO_OP: Record<VisFilterOption, VisFilterOp> = {
  [VisFilterOption.GLOB]: {op: 'glob', label: 'glob'},
  [VisFilterOption.EQUALS_TO]: {op: '=', label: 'equals to'},
  [VisFilterOption.NOT_EQUALS_TO]: {op: '!=', label: 'not equals to'},
  [VisFilterOption.GREATER_THAN]: {op: '>', label: 'greather than'},
  [VisFilterOption.GREATER_OR_EQUALS_THAN]: {
    op: '>=',
    label: 'greather or equals than',
  },
  [VisFilterOption.LESS_THAN]: {op: '<', label: 'less than'},
  [VisFilterOption.LESS_OR_EQUALS_THAN]: {op: '<=', label: 'less than'},
  [VisFilterOption.IS_NULL]: {op: 'IS NULL', label: 'is null'},
  [VisFilterOption.IS_NOT_NULL]: {op: 'IS NOT NULL', label: 'is not null'},
  [VisFilterOption.IN]: {op: 'IN', label: 'in'},
  [VisFilterOption.BETWEEN]: {op: 'BETWEEN', label: 'between'},
};

export function buildFilterSqlClause(filters: VisFilter[]) {
  return filters.map((filter) => `${filterToSql(filter)}`).join(' AND');
}

function filterToSql(filter: VisFilter) {
  const {filterOption, columnName, value} = filter;

  let filterValue: ColumnType | undefined;
  if (value !== undefined) {
    if (Array.isArray(value)) {
      if (filterOption === VisFilterOption.IN) {
        filterValue = '(';
        value.forEach(
          (v, i) => (filterValue += `${v} ${i < value.length - 1 ? ',' : ''}`),
        );
        filterValue += ')';
      }

      if (filterOption === VisFilterOption.BETWEEN) {
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

  return `${columnName} ${VIS_FILTER_OPTION_TO_OP[filterOption].op} ${filterValue === undefined ? filterValue : ''}`;
}

export function chartInteractionToVisFilter(
  selectionType: VegaLiteSelectionTypes,
  fieldName: string,
  fieldType: VegaLiteFieldType,
  filterVal: Item | SignalValue,
) {
  const visFilter = {
    columnName: fieldName,
    value: filterVal,
    filterOption: VisFilterOption.GLOB,
  };

  if (selectionType === 'interval') {
    if (fieldType === 'nominal') {
      visFilter.filterOption = VisFilterOption.IN;
    } else if (fieldType === 'quantitative') {
      visFilter.filterOption = VisFilterOption.BETWEEN;
    }
  } else if (selectionType === 'point') {
    visFilter.filterOption = VisFilterOption.EQUALS_TO;
  }

  return visFilter;
}
