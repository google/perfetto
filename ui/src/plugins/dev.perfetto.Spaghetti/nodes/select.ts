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
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import {ColumnDef} from '../graph_utils';

export interface SelectEntry {
  readonly column: string;
  readonly alias: string;
}

export interface SelectExpression {
  readonly expression: string;
  readonly alias: string;
}

export interface SelectConfig {
  readonly entries: SelectEntry[];
  readonly expressions: SelectExpression[];
}

function exprAlias(e: SelectExpression): string {
  if (e.alias) return e.alias;
  // Use expression text sanitized as a column name.
  if (e.expression) return e.expression.replace(/[^a-zA-Z0-9_]/g, '_');
  return '';
}

// A tiny tag that expands into a text input when clicked.
// If blurred with an empty value, collapses back to the tag.
function AliasTag(): m.Component<{
  alias: string;
  placeholder: string;
  onChange: (value: string) => void;
}> {
  let editing = false;

  return {
    view({attrs: {alias, placeholder, onChange}}) {
      if (editing || alias) {
        return m('.pf-qb-alias-tag', [
          m('span', {style: {opacity: 0.5, fontSize: '11px'}}, 'as'),
          m(TextInput, {
            placeholder,
            value: alias,
            autofocus: editing && !alias,
            onChange: (value: string) => onChange(value),
            onblur: () => {
              if (!alias) editing = false;
            },
          }),
        ]);
      }
      return m(Button, {
        icon: 'shoppingmode',
        compact: true,
        className: 'pf-qb-alias-btn',
        title: 'Add alias',
        onclick: () => {
          editing = true;
        },
      });
    },
  };
}

function SelectNodeContent(): m.Component<{
  config: SelectConfig;
  updateConfig: (updates: Partial<SelectConfig>) => void;
  ctx: RenderContext;
}> {
  let draggingEntry = false;
  let draggingExpr = false;
  let binHover = false;

  function makeDragBin(
    onDrop: (fromIdx: number) => void,
    isDragging: boolean,
  ): m.Children {
    if (!isDragging) return null;
    return m(
      '.pf-qb-drag-bin',
      {
        className: binHover ? 'pf-drag-bin-hover' : '',
        ondragover: (e: DragEvent) => {
          e.preventDefault();
          e.dataTransfer!.dropEffect = 'move';
          binHover = true;
        },
        ondragleave: () => {
          binHover = false;
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          binHover = false;
          draggingEntry = false;
          draggingExpr = false;
          const data = e.dataTransfer!.getData('text/plain');
          const idx = parseInt(data.includes(':') ? data.split(':')[1] : data);
          onDrop(idx);
        },
      },
      m(Icon, {icon: 'delete'}),
    );
  }

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const entries = config.entries;

      return m('.pf-qb-stack', [
        m('.pf-qb-section-label', 'Columns'),
        entries.length === 0 &&
          m(
            '.pf-qb-passthrough-hint',
            {
              style: {
                opacity: 0.5,
                fontStyle: 'italic',
                fontSize: '11px',
                padding: '2px 4px',
              },
            },
            'All columns (passthrough)',
          ),
        m('.pf-qb-filter-list', [
          ...entries.map((entry, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: `entry:${i}`,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `entry:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingEntry = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                  draggingEntry = false;
                  binHover = false;
                },
                ondragover: (e: DragEvent) => {
                  e.preventDefault();
                  e.dataTransfer!.dropEffect = 'move';
                  const el = e.currentTarget as HTMLElement;
                  const rect = el.getBoundingClientRect();
                  const isBottom = e.clientY > rect.top + rect.height / 2;
                  el.classList.toggle('pf-drag-over-top', !isBottom);
                  el.classList.toggle('pf-drag-over-bottom', isBottom);
                },
                ondragleave: (e: DragEvent) => {
                  const el = e.currentTarget as HTMLElement;
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                  const data = e.dataTransfer!.getData('text/plain');
                  if (!data.startsWith('entry:')) return;
                  const fromIdx = parseInt(data.slice(6));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...entries];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({entries: updated});
                  }
                },
              },
              [
                m(Icon, {
                  icon: 'drag_indicator',
                  className: 'pf-qb-drag-handle',
                }),
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
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Add column',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () => {
              updateConfig({
                entries: [...entries, {column: '', alias: ''}],
              });
            },
          }),
          makeDragBin(
            (idx) =>
              updateConfig({
                entries: entries.filter((_, j) => j !== idx),
              }),
            draggingEntry,
          ),
        ]),

        m('.pf-qb-section-label', 'Expressions'),
        m('.pf-qb-filter-list', [
          ...config.expressions.map((expr, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: `expr:${i}`,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `expr:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingExpr = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                  draggingExpr = false;
                  binHover = false;
                },
                ondragover: (e: DragEvent) => {
                  e.preventDefault();
                  e.dataTransfer!.dropEffect = 'move';
                  const el = e.currentTarget as HTMLElement;
                  const rect = el.getBoundingClientRect();
                  const isBottom = e.clientY > rect.top + rect.height / 2;
                  el.classList.toggle('pf-drag-over-top', !isBottom);
                  el.classList.toggle('pf-drag-over-bottom', isBottom);
                },
                ondragleave: (e: DragEvent) => {
                  const el = e.currentTarget as HTMLElement;
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                  const data = e.dataTransfer!.getData('text/plain');
                  if (!data.startsWith('expr:')) return;
                  const fromIdx = parseInt(data.slice(5));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...config.expressions];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({expressions: updated});
                  }
                },
              },
              [
                m(Icon, {
                  icon: 'drag_indicator',
                  className: 'pf-qb-drag-handle',
                }),
                m(TextInput, {
                  placeholder: 'expression',
                  value: expr.expression,
                  onChange: (value: string) => {
                    const newExprs = [...config.expressions];
                    newExprs[i] = {...expr, expression: value};
                    updateConfig({expressions: newExprs});
                  },
                }),
                m(AliasTag, {
                  alias: expr.alias,
                  placeholder: exprAlias(expr) || 'alias',
                  onChange: (value: string) => {
                    const newExprs = [...config.expressions];
                    newExprs[i] = {...expr, alias: value};
                    updateConfig({expressions: newExprs});
                  },
                }),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Add expression',
            variant: ButtonVariant.Filled,
            icon: 'add',
            onclick: () => {
              updateConfig({
                expressions: [
                  ...config.expressions,
                  {expression: '', alias: ''},
                ],
              });
            },
          }),
          makeDragBin(
            (idx) =>
              updateConfig({
                expressions: config.expressions.filter((_, j) => j !== idx),
              }),
            draggingExpr,
          ),
        ]),
      ]);
    },
  };
}

function getOutputColumns(
  config: SelectConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const hasEntries = config.entries.some((e) => e.column);
  const exprAliases: ColumnDef[] = config.expressions
    .filter((e) => e.expression)
    .map((e) => ({name: exprAlias(e)}));

  // No explicit columns: pass through all input columns (SELECT *)
  // plus any expressions on top.
  if (!hasEntries) {
    return [...(columns ?? []), ...exprAliases];
  }

  const selectedEntries: ColumnDef[] = config.entries
    .filter((e) => e.column)
    .map((e) => {
      const name = e.alias || e.column;
      const orig = columns?.find((c) => c.name === e.column);
      return {name, type: orig?.type};
    });
  return [...selectedEntries, ...exprAliases];
}

function isValid(config: SelectConfig): boolean {
  return config.expressions.every((e) => !e.alias || e.expression);
}

function tryFold(stmt: SqlStatement, config: SelectConfig): boolean {
  if (stmt.columns !== '*') return false;
  const entryParts = config.entries
    .filter((e) => e.column)
    .map((e) => (e.alias ? `${e.column} AS ${e.alias}` : e.column));
  const exprParts = config.expressions
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
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 145,
  defaultConfig: () => ({entries: [], expressions: []}),
  render(config, updateConfig, ctx) {
    return m(SelectNodeContent, {config, updateConfig, ctx});
  },
  getOutputColumns,
  isValid,
  tryFold,
};
