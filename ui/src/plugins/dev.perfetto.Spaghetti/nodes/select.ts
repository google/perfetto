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
import {
  ColumnContext,
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {MultiSelectDiff, PopupMultiSelect} from '../../../widgets/multiselect';
import {TextInput} from '../../../widgets/text_input';
import {ColumnDef} from '../graph_utils';

export interface SelectExpression {
  readonly expression: string;
  readonly alias: string;
}

export interface SelectConfig {
  readonly columns: Record<string, boolean>;
  readonly expressions: SelectExpression[];
}

function renderSelectNode(
  config: SelectConfig,
  updateConfig: (updates: Partial<SelectConfig>) => void,
  ctx: RenderContext,
): m.Children {
  const availableColumns = ctx.availableColumns;

  if (availableColumns.length === 0) {
    return m('span.pf-qb-placeholder', 'Connect to a table source');
  }

  // Merge available columns with current selection state.
  // Columns that exist upstream but aren't in config.columns yet default to true.
  const mergedColumns: Record<string, boolean> = {};
  for (const col of availableColumns) {
    mergedColumns[col.name] =
      col.name in config.columns ? config.columns[col.name] : true;
  }

  const options = availableColumns.map((col) => ({
    id: col.name,
    name: col.name,
    checked: mergedColumns[col.name],
  }));

  return m('.pf-qb-stack', [
    m(PopupMultiSelect, {
      label: 'Columns',
      showNumSelected: true,
      options,
      onChange: (diffs: MultiSelectDiff[]) => {
        const updated = {...mergedColumns};
        for (const diff of diffs) {
          updated[diff.id] = diff.checked;
        }
        updateConfig({columns: updated});
      },
    }),
    m('.pf-qb-section-label', 'Expressions'),
    m('.pf-qb-3col-grid', [
      ...config.expressions.flatMap((expr, i) => [
        m(TextInput, {
          placeholder: 'expression',
          value: expr.expression,
          onChange: (value: string) => {
            const newExprs = [...config.expressions];
            newExprs[i] = {...expr, expression: value};
            updateConfig({expressions: newExprs});
          },
        }),
        m(TextInput, {
          placeholder: 'alias',
          value: expr.alias,
          onChange: (value: string) => {
            const newExprs = [...config.expressions];
            newExprs[i] = {...expr, alias: value};
            updateConfig({expressions: newExprs});
          },
        }),
        m(Button, {
          icon: 'delete',
          intent: Intent.Danger,
          title: 'Remove expression',
          onclick: () => {
            const newExprs = config.expressions.filter((_, j) => j !== i);
            updateConfig({expressions: newExprs});
          },
        }),
      ]),
    ]),
    m(Button, {
      label: 'Add expression',
      variant: ButtonVariant.Filled,
      icon: 'add',
      onclick: () => {
        updateConfig({
          expressions: [...config.expressions, {expression: '', alias: ''}],
        });
      },
    }),
  ]);
}

function getOutputColumns(
  config: SelectConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const selected = Object.entries(config.columns)
    .filter(([_, checked]) => checked)
    .map(([col]) => columns?.find((c) => c.name === col) ?? {name: col});
  const exprAliases: ColumnDef[] = config.expressions
    .filter((e) => e.alias && e.expression)
    .map((e) => ({name: e.alias}));
  if (selected.length > 0) {
    return [...selected, ...exprAliases];
  } else if (exprAliases.length > 0) {
    return [...(columns ?? []), ...exprAliases];
  }
  return columns;
}

function isValid(config: SelectConfig): boolean {
  return config.expressions.every(
    (e) => (!e.expression && !e.alias) || (e.expression && e.alias),
  );
}

function tryFold(stmt: SqlStatement, config: SelectConfig): boolean {
  if (stmt.columns !== '*') return false;
  const selectedCols = Object.entries(config.columns)
    .filter(([_, checked]) => checked)
    .map(([col]) => col);
  const exprParts = config.expressions
    .filter((e) => e.expression && e.alias)
    .map((e) => `${e.expression} AS ${e.alias}`);
  if (selectedCols.length > 0) {
    stmt.columns = [...selectedCols, ...exprParts].join(', ');
  } else if (exprParts.length > 0) {
    stmt.columns = ['*', ...exprParts].join(', ');
  }
  return true;
}

export const manifest: NodeManifest<SelectConfig> = {
  title: 'Select',
  icon: 'view_column',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 145,
  defaultConfig: () => ({columns: {}, expressions: []}),
  render: renderSelectNode,
  getOutputColumns,
  isValid,
  tryFold,
};
