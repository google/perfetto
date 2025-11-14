// Copyright (C) 2025 The Android Open Source Project
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

import m from 'mithril';
import {MenuItem} from '../../../../../widgets/menu';
import {Form} from '../../../../../widgets/form';
import {TextInput} from '../../../../../widgets/text_input';
import {SqlValue} from '../../../../../trace_processor/query_result';
import {
  isQuantitativeType,
  PerfettoSqlType,
} from '../../../../../trace_processor/perfetto_sql_type';
import {SqlTableState} from '../state';
import {TableColumn} from '../table_column';
import {sqlValueToSqliteString} from '../../../../../trace_processor/sql_utils';
import {Result, errResult, okResult} from '../../../../../base/result';

type FilterParams = {
  op: string;
  label: string;
  supported?: (type: PerfettoSqlType) => boolean;
  placeholder?: string;
};

const FILTERS = {
  'glob': {
    op: 'glob',
    label: 'glob',
    supported: (type: PerfettoSqlType) => type.kind === 'string',
    placeholder: '*pattern*',
  },
  'equals to': {op: '=', label: 'equals to'},
  'not equals to': {op: '!=', label: 'not equals to'},
  'greater than': {
    op: '>',
    label: 'greater than',
    supported: isQuantitativeType,
  },
  'greater or equals than': {
    op: '>=',
    label: 'greater or equals than',
    supported: isQuantitativeType,
  },
  'less than': {op: '<', label: 'less than', supported: isQuantitativeType},
  'less or equals than': {
    op: '<=',
    label: 'less or equals than',
    supported: isQuantitativeType,
  },
} satisfies Record<string, FilterParams>;

type FilterLabel = keyof typeof FILTERS;

interface ColumnFilterAttrs {
  filter: FilterLabel;
  params: FilterParams;
  column: TableColumn;
  state: SqlTableState;
}

// Separating out an individual column filter into a class
// so that we can store the raw input value
class ParametrizedColumnFilter implements m.ClassComponent<ColumnFilterAttrs> {
  // Holds the raw string value from the filter text input element
  private inputValue: string;
  private error: boolean = false;

  constructor() {
    this.inputValue = '';
  }

  view({attrs}: m.Vnode<ColumnFilterAttrs>) {
    const {filter: filterOption, params, column, state} = attrs;

    return m(
      MenuItem,
      {
        label: filterOption,
      },
      m(
        Form,
        {
          onSubmit: (e: Event) => {
            if (this.inputValue === '') return;
            this.error = false;

            const parseResult =
              column.type !== undefined
                ? this.parseValueByType(this.inputValue, column.type)
                : okResult(this.autoDetectValue(this.inputValue));

            if (!parseResult.ok) {
              // The form should not be submitted if the input is not valid.
              e.stopPropagation();
              this.error = true;
              m.redraw();
              return;
            }

            const filterValue = sqlValueToSqliteString(parseResult.value);
            state.filters.addFilter({
              op: (cols) => `${cols[0]} ${params.op} ${filterValue}`,
              columns: [column.column],
            });
          },
          submitLabel: 'Filter',
        },
        [
          m(TextInput, {
            id: 'column_filter_value',
            ref: 'COLUMN_FILTER_VALUE',
            autofocus: true,
            placeholder:
              params.placeholder ?? this.getPlaceholderForType(column.type),
            oninput: (e: InputEvent) => {
              if (!e.target) return;
              this.inputValue = (e.target as HTMLInputElement).value;
              this.error = false;
            },
            style: this.error
              ? {
                  border: '1px solid red',
                  outline: 'none',
                }
              : undefined,
          }),
        ],
      ),
    );
  }

  private parseValueByType(
    value: string,
    type: PerfettoSqlType,
  ): Result<SqlValue> {
    switch (type.kind) {
      case 'int':
      case 'id':
      case 'joinid':
      case 'arg_set_id':
      case 'timestamp':
      case 'duration':
        // Validate that the value can be parsed as BigInt
        if (!/^-?\d+$/.test(value.trim())) {
          return errResult(`Invalid integer value. Expected a whole number.`);
        }
        try {
          return okResult(BigInt(value));
        } catch (e) {
          return errResult(`Integer value out of range: ${e}`);
        }
      case 'double':
        const numValue = Number(value);
        if (isNaN(numValue)) {
          return errResult(`Invalid number value. Expected a numeric value.`);
        }
        return okResult(numValue);
      case 'boolean':
        // For boolean types, parse as boolean
        const lowerValue = value.toLowerCase();
        if (
          lowerValue !== 'true' &&
          lowerValue !== 'false' &&
          lowerValue !== '1' &&
          lowerValue !== '0'
        ) {
          return errResult(
            `Invalid boolean value. Expected 'true', 'false', '1', or '0'.`,
          );
        }
        return okResult(lowerValue === 'true' || lowerValue === '1' ? 1 : 0);
      case 'string':
      case 'bytes':
        return okResult(value);
      default:
        // Fallback to auto-detection
        return okResult(this.autoDetectValue(value));
    }
  }

  private autoDetectValue(value: string): SqlValue {
    if (Number.isNaN(Number.parseFloat(value))) {
      return value;
    } else if (!Number.isInteger(Number.parseFloat(value))) {
      return Number(value);
    } else {
      return BigInt(value);
    }
  }

  private getPlaceholderForType(type?: PerfettoSqlType): string {
    if (!type) {
      return 'value';
    }

    switch (type.kind) {
      case 'int':
      case 'id':
      case 'joinid':
      case 'arg_set_id':
        return 'integer...';
      case 'double':
        return 'number...';
      case 'boolean':
        return 'true or false';
      case 'string':
      case 'bytes':
        return 'text...';
      case 'timestamp':
        return 'timestamp (ns)...';
      case 'duration':
        return 'duration (ns)...';
    }
  }
}

export function renderColumnFilterOptions(
  column: TableColumn,
  state: SqlTableState,
): m.Children {
  const filterMenuItems = Object.entries(FILTERS)
    .filter((item) => {
      const params: FilterParams = item[1];
      if (column.type === undefined || params.supported === undefined) {
        return true;
      }
      return params.supported(column.type);
    })
    .map(([filter, params]) =>
      m(ParametrizedColumnFilter, {
        filter: filter as FilterLabel,
        params,
        column,
        state: state,
      }),
    );

  const nullMenuItems = ['is null', 'is not null'].map((label) =>
    m(MenuItem, {
      label,
      onclick: () => {
        state.filters.addFilter({
          op: (cols) => `${cols[0]} ${label}`,
          columns: [column.column],
        });
      },
    }),
  );

  return [...filterMenuItems, ...nullMenuItems];
}
