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
import type {
  ColumnContext,
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {TextInput} from '../../../widgets/text_input';
import {moveItem, Row} from '../components/row';
import {AddButton} from '../components/add_button';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';
import {AliasTag} from '../components/alias_tag';

export interface SelectEntry {
  readonly column: string;
  readonly alias: string;
}

export interface SelectExpression {
  readonly expression: string;
  readonly alias: string;
}

export interface SelectConfig {
  readonly entries?: SelectEntry[];
  readonly expressions?: SelectExpression[];
}

function exprAlias(e: SelectExpression): string {
  if (e.alias) return e.alias;
  // Use expression text sanitized as a column name.
  if (e.expression) return e.expression.replace(/[^a-zA-Z0-9_]/g, '_');
  return '';
}

function SelectNodeContent(): m.Component<{
  config: SelectConfig;
  updateConfig: (updates: Partial<SelectConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const entries = config.entries ?? [];
      const expressions = config.expressions ?? [];

      return m(Stack, [
        m('.pf-spag-section-label', 'Columns'),
        entries.length === 0 &&
          m('.pf-spag-placeholder', 'All columns (passthrough)'),
        m(Stack, {compact: true}, [
          ...entries.map((entry, i) =>
            m(
              Row,
              {
                key: `entry:${i}`,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({entries: moveItem(entries, from, to)});
                  },
                },
              },
              [
                m(ColumnPicker, {
                  value: entry.column,
                  columns: availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...entries];
                    updated[i] = {...entry, column: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: entry.column || 'alias',
                  onChange: (value: string) => {
                    const updated = [...entries];
                    updated[i] = {...entry, alias: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      entries: entries.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(AddButton, {
          label: 'Add column',
          onclick: () => {
            updateConfig({
              entries: [...entries, {column: '', alias: ''}],
            });
          },
        }),

        m('.pf-spag-section-label', 'Expressions'),
        m(Stack, {compact: true}, [
          ...expressions.map((expr, i) =>
            m(
              Row,
              {
                key: `expr:${i}`,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({
                      expressions: moveItem(expressions, from, to),
                    });
                  },
                },
              },
              [
                m(TextInput, {
                  placeholder: 'expression',
                  value: expr.expression,
                  onChange: (value: string) => {
                    const newExprs = [...expressions];
                    newExprs[i] = {...expr, expression: value};
                    updateConfig({expressions: newExprs});
                  },
                }),
                m(AliasTag, {
                  alias: expr.alias,
                  placeholder: exprAlias(expr) || 'alias',
                  onChange: (value: string) => {
                    const newExprs = [...expressions];
                    newExprs[i] = {...expr, alias: value};
                    updateConfig({expressions: newExprs});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      expressions: expressions.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(AddButton, {
          label: 'Add expression',
          onclick: () => {
            updateConfig({
              expressions: [...expressions, {expression: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function getOutputColumns(
  config: SelectConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const entries = config.entries ?? [];
  const expressions = config.expressions ?? [];
  const hasEntries = entries.some((e) => e.column);
  const exprAliases: ColumnDef[] = expressions
    .filter((e) => e.expression)
    .map((e) => ({name: exprAlias(e)}));

  // No explicit columns: pass through all input columns (SELECT *)
  // plus any expressions on top.
  if (!hasEntries) {
    return [...(columns ?? []), ...exprAliases];
  }

  const selectedEntries: ColumnDef[] = entries
    .filter((e) => e.column)
    .map((e) => {
      const name = e.alias || e.column;
      const orig = columns?.find((c) => c.name === e.column);
      return {name, type: orig?.type};
    });
  return [...selectedEntries, ...exprAliases];
}

function isValid(config: SelectConfig): boolean {
  const expressions = config.expressions ?? [];
  return expressions.every((e) => !e.alias || !!e.expression);
}

function tryFold(stmt: SqlStatement, config: SelectConfig): boolean {
  const entries = config.entries ?? [];
  const expressions = config.expressions ?? [];
  if (stmt.columns !== '*') return false;
  const entryParts = entries
    .filter((e) => e.column)
    .map((e) => (e.alias ? `${e.column} AS ${e.alias}` : e.column));
  const exprParts = expressions
    .filter((e) => e.expression)
    .map((e) => `${e.expression} AS ${exprAlias(e)}`);

  // No explicit columns: keep * and append expressions on top.
  if (entryParts.length === 0) {
    if (exprParts.length > 0) {
      stmt.columns = `*, ${exprParts.join(', ')}`;
    }
    return true;
  }

  stmt.columns = [...entryParts, ...exprParts].join(', ');
  return true;
}

export const manifest: NodeManifest<SelectConfig> = {
  title: 'Select',
  icon: 'view_column',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 145,
  defaultConfig: () => ({entries: [], expressions: []}),
  render(config, updateConfig, ctx) {
    return m(SelectNodeContent, {config, updateConfig, ctx});
  },
  getOutputColumns,
  isValid,
  tryFold,
};
