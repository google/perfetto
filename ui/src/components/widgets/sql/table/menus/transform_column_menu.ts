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
import {Form, FormLabel} from '../../../../../widgets/form';
import {TextInput} from '../../../../../widgets/text_input';
import {Icons} from '../../../../../base/semantic_icons';
import {TableColumn, RenderedCell, TableManager} from '../table_column';
import {SqlTableState} from '../state';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
  typesEqual,
} from '../../../../../trace_processor/perfetto_sql_type';
import {createTableColumn} from '../create_column';
import {SqlColumn, SqlExpression} from '../sql_column';
import {SqlValue} from '../../../../../trace_processor/query_result';
import {uuidv4} from '../../../../../base/uuid';
import {range} from '../../../../../base/array_utils';

type Transform = {
  // The SQL expresssion to apply.
  expression: (colExpr: string, ...params: string[]) => string;
  // Optional parameters for the transform
  parameters?: TransformParameter[];
  requiredType?: PerfettoSqlType;
  resultType: PerfettoSqlType;
};

type TransformParameter = {
  name: string;
  placeholder: string;
  defaultValue?: string;
  validate?: (value: string) => boolean;
};

const TRANSFORMS = {
  'length': {
    expression: (col) => `length(${col})`,
    requiredType: PerfettoSqlTypes.STRING,
    resultType: {kind: 'int'},
  },
  'substring': {
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
    requiredType: PerfettoSqlTypes.STRING,
    resultType: PerfettoSqlTypes.STRING,
  },
  'extract regex': {
    expression: (col, pattern) => `regexp_extract(${col}, '${pattern}')`,
    parameters: [
      {
        name: 'pattern',
        placeholder: 'regex pattern (e.g., \\d+)',
      },
    ],
    requiredType: PerfettoSqlTypes.STRING,
    resultType: PerfettoSqlTypes.STRING,
  },
  'strip prefix': {
    expression: (col, prefix) =>
      `CASE WHEN ${col} GLOB '${prefix}*' THEN substr(${col}, ${prefix.length + 1}) ELSE ${col} END`,
    parameters: [
      {
        name: 'prefix',
        placeholder: 'prefix to remove',
      },
    ],
    requiredType: PerfettoSqlTypes.STRING,
    resultType: PerfettoSqlTypes.STRING,
  },
  'strip suffix': {
    expression: (col, suffix) =>
      `CASE WHEN ${col} GLOB '*${suffix}' THEN substr(${col}, 1, length(${col}) - ${suffix.length}) ELSE ${col} END`,
    parameters: [
      {
        name: 'suffix',
        placeholder: 'suffix to remove',
      },
    ],
    requiredType: PerfettoSqlTypes.STRING,
    resultType: PerfettoSqlTypes.STRING,
  },
} satisfies Record<string, Transform>;

type TransformType = keyof typeof TRANSFORMS;

export class TransformColumn implements TableColumn {
  public readonly column: SqlColumn;
  public readonly type: PerfettoSqlType | undefined;
  constructor(
    public readonly args: {
      transformed: TableColumn;
      source: TableColumn;
      transformType: TransformType;
      transformParams: string[];
      state: SqlTableState;
    },
  ) {
    this.column = args.transformed.column;
    this.type = TRANSFORMS[args.transformType].resultType;
  }

  getTitle(): string | undefined {
    return this.args.transformed.getTitle?.();
  }

  renderCell(value: SqlValue, tableManager?: TableManager): RenderedCell {
    return this.args.transformed.renderCell(value, tableManager);
  }

  listDerivedColumns(manager: TableManager) {
    return this.args.transformed.listDerivedColumns?.(manager);
  }

  getColumnSpecificMenuItems(args: {
    replaceColumn: (column: TableColumn) => void;
  }): m.Children {
    return [
      this.args.transformParams.length !== 0 &&
        m(
          MenuItem,
          {
            label: 'Edit transform',
            icon: Icons.Edit,
          },
          m(ConfigureTransformMenu, {
            column: this.args.source,
            state: this.args.state,
            transformType: this.args.transformType,
            initialValues: this.args.transformParams,
            onApply: (newColumn: TableColumn) => args.replaceColumn(newColumn),
            formSubmitLabel: 'Edit',
          }),
        ),
      m(MenuItem, {
        label: 'Undo transform',
        icon: Icons.Undo,
        onclick: () => args.replaceColumn(this.args.source),
      }),
    ];
  }
}

function applyTransform(args: {
  column: TableColumn;
  transformType: TransformType;
  values: string[];
  state: SqlTableState;
}): TableColumn {
  const transform: Transform = TRANSFORMS[args.transformType];
  const values = args.values;
  const transformExpression = (cols: string[]) =>
    transform.expression(cols[0], ...values);

  return new TransformColumn({
    source: args.column,
    transformed: createTableColumn({
      trace: args.state.trace,
      column: new SqlExpression(transformExpression, [args.column.column]),
      type: transform.resultType,
    }),
    state: args.state,
    transformType: args.transformType,
    transformParams: args.values,
  });
}

interface TransformMenuItemAttrs {
  column: TableColumn;
  state: SqlTableState;
  transformType: TransformType;
  initialValues?: string[];
  onApply: (newColumn: TableColumn) => void;
  formSubmitLabel: string;
}

class ConfigureTransformMenu
  implements m.ClassComponent<TransformMenuItemAttrs>
{
  private paramState: {value: string; error: boolean}[] = [];
  private readonly uuid = uuidv4();

  view({attrs}: m.Vnode<TransformMenuItemAttrs>) {
    const transform: Transform = TRANSFORMS[attrs.transformType];
    const params = transform.parameters ?? [];
    const initialValues = attrs.initialValues ?? [];
    if (this.paramState.length !== params.length) {
      this.paramState = range(params.length).map((index) => {
        if (index < initialValues.length) {
          return {value: initialValues[index], error: false};
        }
        return {value: '', error: false};
      });
    }

    return m(
      Form,
      {
        submitLabel: attrs.formSubmitLabel,
        onSubmit: (e: Event) => {
          params.forEach((param, index) => {
            const value = this.paramState[index].value;
            this.paramState[index].error = !(param.validate?.(value) ?? true);
          });

          const hasError = this.paramState.some((state) => state.error);
          if (!hasError) {
            attrs.onApply(
              applyTransform({
                column: attrs.column,
                state: attrs.state,
                transformType: attrs.transformType,
                values: params.map((param, index) => {
                  const value = this.paramState[index].value;
                  if (value === '' && param.defaultValue !== undefined) {
                    return param.defaultValue;
                  }
                  return value;
                }),
              }),
            );
          } else {
            e.stopPropagation();
          }
        },
      },
      params.map((param, index) => [
        params.length > 1 &&
          m(FormLabel, {for: `${this.uuid}_param_${index}`}, param.name),
        m(TextInput, {
          id: `${this.uuid}_param_${index}`,
          placeholder: param.placeholder,
          value: this.paramState[index].value,
          oninput: (e: InputEvent) => {
            this.paramState[index].value = (e.target as HTMLInputElement).value;
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
    );
  }
}

export function renderTransformColumnMenu(
  column: TableColumn,
  columnIndex: number,
  state: SqlTableState,
): m.Children {
  const applicableTransforms = (
    Object.entries(TRANSFORMS) as [TransformType, Transform][]
  ).filter(
    ([_, transform]) =>
      transform.requiredType === undefined ||
      (column.type !== undefined &&
        typesEqual(transform.requiredType, column.type)),
  );

  // Only show the Transform menu if there are applicable transformations
  if (applicableTransforms.length === 0) {
    return null;
  }

  return m(
    MenuItem,
    {label: 'Transform', icon: Icons.ApplyFunction},
    applicableTransforms.map(([name, transform]) => {
      const paramCount = transform.parameters?.length ?? 0;
      return m(
        MenuItem,
        {
          label: name,
          onclick:
            paramCount === 0
              ? () =>
                  state.addColumn(
                    applyTransform({
                      column,
                      state,
                      transformType: name,
                      values: [],
                    }),
                    columnIndex,
                  )
              : undefined,
        },
        paramCount !== 0 &&
          m(ConfigureTransformMenu, {
            column,
            state,
            transformType: name,
            onApply: (column: TableColumn) =>
              state.addColumn(column, columnIndex),
            formSubmitLabel: 'Add',
          }),
      );
    }),
  );
}
