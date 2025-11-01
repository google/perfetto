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

import m from 'mithril';
import {MenuItem} from '../../../../../widgets/menu';
import {Form, FormLabel} from '../../../../../widgets/form';
import {TextInput} from '../../../../../widgets/text_input';
import {Icons} from '../../../../../base/semantic_icons';
import {TableColumn} from '../table_column';
import {SqlTableState} from '../state';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../../../trace_processor/perfetto_sql_type';
import {createTableColumn} from '../create_column';
import {SqlExpression} from '../sql_column';
import {uuidv4} from '../../../../../base/uuid';
import {range} from '../../../../../base/array_utils';

type Transform = {
  label: string;
  // The SQL expresssion to apply.
  expression: (colExpr: string, ...params: string[]) => string;
  // Optional parameters for the transform
  parameters?: TransformParameter[];
  resultType: PerfettoSqlType;
};

type TransformParameter = {
  name: string;
  placeholder: string;
  defaultValue?: string;
  validate?: (value: string) => boolean;
};

const STRING_TRANSFORMS: Transform[] = [
  {
    label: 'length',
    expression: (col) => `length(${col})`,
    resultType: {kind: 'int'},
  },
  {
    label: 'substring',
    expression: (col, start, length) => {
      if (length) {
        return `substr(${col}, ${start}, ${length})`;
      }
      return `substr(${col}, ${start})`;
    },
    parameters: [
      {
        name: 'start',
        placeholder: '1-based, can be negative (optional)',
        defaultValue: '1',
        validate: (value) => {
          if (value === '') {
            return true;
          }
          const num = parseInt(value);
          return !isNaN(num);
        },
      },
      {
        name: 'length',
        placeholder: 'optional',
        validate: (value) => {
          if (value === '') {
            return true;
          }
          const num = parseInt(value);
          return !isNaN(num) && num > 0;
        },
      },
    ],
    resultType: PerfettoSqlTypes.STRING,
  },
  {
    label: 'extract regex',
    expression: (col, pattern) => `regexp_extract(${col}, '${pattern}')`,
    parameters: [
      {
        name: 'pattern',
        placeholder: 'regex pattern (e.g., \\d+)',
      },
    ],
    resultType: PerfettoSqlTypes.STRING,
  },
  {
    label: 'strip prefix',
    expression: (col, prefix) =>
      `CASE WHEN ${col} LIKE '${prefix}%' THEN substr(${col}, ${prefix.length + 1}) ELSE ${col} END`,
    parameters: [
      {
        name: 'prefix',
        placeholder: 'prefix to remove',
      },
    ],
    resultType: PerfettoSqlTypes.STRING,
  },
  {
    label: 'strip suffix',
    expression: (col, suffix) =>
      `CASE WHEN ${col} LIKE '%${suffix}' THEN substr(${col}, 1, length(${col}) - ${suffix.length}) ELSE ${col} END`,
    parameters: [
      {
        name: 'suffix',
        placeholder: 'suffix to remove',
      },
    ],
    resultType: PerfettoSqlTypes.STRING,
  },
];

interface TransformMenuItemAttrs {
  column: TableColumn;
  columnIndex: number;
  state: SqlTableState;
  transform: Transform;
}

class TransformMenuItem implements m.ClassComponent<TransformMenuItemAttrs> {
  private paramState: {value: string; error: boolean}[] = [];
  private readonly uuid = uuidv4();

  view({attrs}: m.Vnode<TransformMenuItemAttrs>) {
    const {transform} = attrs;
    const params = transform.parameters ?? [];

    if (params.length === 0) {
      // If there are no parameters, apply this directly.
      return m(MenuItem, {
        label: transform.label,
        onclick: () => this.applyTransform(attrs),
      });
    }

    if (params.length !== this.paramState.length) {
      this.paramState = range(params.length).map(() => {
        return {value: '', error: false};
      });
    }

    return m(
      MenuItem,
      {label: transform.label},
      m(
        Form,
        {
          onSubmit: (e: Event) => {
            e.stopPropagation();
            params.forEach((param, index) => {
              const value = this.paramState[index].value;
              this.paramState[index].error = !(param.validate?.(value) ?? true);
            });
            const hasError = this.paramState.some((state) => state.error);
            if (hasError) {
              e.stopPropagation();
            } else {
              this.applyTransform(attrs);
            }
          },
          submitLabel: 'Apply',
        },
        params.map((param, index) => [
          params.length > 1 &&
            m(FormLabel, {for: `${this.uuid}_param_${index}`}, param.name),
          m(TextInput, {
            id: `${this.uuid}_param_${index}`,
            placeholder: param.placeholder,
            value: this.paramState[index].value,
            oninput: (e: InputEvent) => {
              this.paramState[index].value = (
                e.target as HTMLInputElement
              ).value;
              this.paramState[index].error = false;
            },
            style: this.paramState[index].error
              ? {
                  border: '1px solid red',
                  outline: 'none',
                }
              : {},
          }),
        ]),
      ),
    );
  }

  private applyTransform(attrs: TransformMenuItemAttrs) {
    const {column, columnIndex, state, transform} = attrs;
    const values = this.paramState.map((state, index) => {
      const defaultValue = attrs.transform.parameters?.[index].defaultValue;
      if (defaultValue !== undefined) {
        return state.value || defaultValue;
      }
      return state.value;
    });

    // Create the transformation expression
    const transformExpression = (cols: string[]) =>
      transform.expression(cols[0], ...values);

    // Create the new column with transformation
    const newColumn = createTableColumn({
      trace: state.trace,
      column: new SqlExpression(transformExpression, [column.column]),
      type: transform.resultType,
      origin: {
        kind: 'transform',
        source: column,
      },
    });

    // Add the new column after the current column
    state.addColumn(newColumn, columnIndex);
  }
}

export function renderTransformColumnMenu(
  column: TableColumn,
  columnIndex: number,
  state: SqlTableState,
): m.Children {
  const transforms: Transform[] = [];

  if (column?.type?.kind === 'string') {
    transforms.push(...STRING_TRANSFORMS);
  }

  // Only show the Transform menu if there are applicable transformations
  if (transforms.length === 0) {
    return null;
  }

  return m(
    MenuItem,
    {label: 'Transform', icon: Icons.ApplyFunction},
    transforms.map((transform) =>
      m(TransformMenuItem, {
        column,
        columnIndex,
        state,
        transform,
      }),
    ),
  );
}
