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

export interface SelectExpression {
  readonly expression: string;
  readonly alias: string;
}

export interface SelectConfig {
  readonly columns: Record<string, boolean>;
  readonly expressions: SelectExpression[];
}

function SelectNodeContent(): m.Component<{
  config: SelectConfig;
  updateConfig: (updates: Partial<SelectConfig>) => void;
  ctx: RenderContext;
}> {
  let draggingCol = false;
  let draggingExpr = false;
  let binHover = false;

  function makeDragBin(
    onDrop: (fromIdx: number) => void,
    isDragging: boolean,
  ): m.Children {
    if (!isDragging) return null;
    return m('.pf-qb-drag-bin', {
      className: binHover ? 'pf-drag-bin-hover' : '',
      ondragover: (e: DragEvent) => {
        e.preventDefault();
        e.dataTransfer!.dropEffect = 'move';
        binHover = true;
      },
      ondragleave: () => { binHover = false; },
      ondrop: (e: DragEvent) => {
        e.preventDefault();
        binHover = false;
        draggingCol = false;
        draggingExpr = false;
        const data = e.dataTransfer!.getData('text/plain');
        const idx = parseInt(data.includes(':') ? data.split(':')[1] : data);
        onDrop(idx);
      },
    }, m(Icon, {icon: 'delete'}));
  }

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;

      if (availableColumns.length === 0) {
        return m('span.pf-qb-placeholder', 'Connect to a table source');
      }

      // Merge: upstream columns not in config default to true.
      const mergedColumns: Record<string, boolean> = {};
      for (const col of availableColumns) {
        mergedColumns[col.name] =
          col.name in config.columns ? config.columns[col.name] : true;
      }

      // Selected columns as an ordered list.
      const selectedCols = availableColumns
        .filter((col) => mergedColumns[col.name])
        .map((col) => col.name);

      // Unselected columns available to add.
      const unselectedCols = availableColumns
        .filter((col) => !mergedColumns[col.name]);

      const removeColumn = (colName: string) => {
        updateConfig({columns: {...mergedColumns, [colName]: false}});
      };

      const addColumn = (colName: string) => {
        updateConfig({columns: {...mergedColumns, [colName]: true}});
      };

      return m('.pf-qb-stack', [
        m('.pf-qb-section-label', 'Columns'),
        m('.pf-qb-filter-list', [
          ...selectedCols.map((colName, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: `col:${colName}`,
                draggable: selectedCols.length > 1,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `col:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingCol = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
                  draggingCol = false;
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
                  el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
                  const data = e.dataTransfer!.getData('text/plain');
                  if (!data.startsWith('col:')) return;
                  const fromIdx = parseInt(data.slice(4));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    // Reorder by rebuilding the columns record in the new order.
                    const reordered = [...selectedCols];
                    const [moved] = reordered.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    reordered.splice(toIdx, 0, moved);
                    const newCols: Record<string, boolean> = {};
                    for (const c of reordered) newCols[c] = true;
                    // Keep deselected columns as false.
                    for (const col of availableColumns) {
                      if (!(col.name in newCols)) newCols[col.name] = false;
                    }
                    updateConfig({columns: newCols});
                  }
                },
              },
              [
                ...(selectedCols.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
                m('span.pf-qb-col-label', colName),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(ColumnPicker, {
            value: '',
            columns: unselectedCols,
            placeholder: 'Add column...',
            onSelect: addColumn,
          }),
          makeDragBin(
            (idx) => removeColumn(selectedCols[idx]),
            draggingCol,
          ),
        ]),

        m('.pf-qb-section-label', 'Expressions'),
        m('.pf-qb-filter-list', [
          ...config.expressions.map((expr, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: `expr:${i}`,
                draggable: config.expressions.length > 1,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `expr:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingExpr = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
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
                  el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
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
                ...(config.expressions.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
                m(TextInput, {
                  placeholder: 'expression',
                  value: expr.expression,
                  onChange: (value: string) => {
                    const newExprs = [...config.expressions];
                    newExprs[i] = {...expr, expression: value};
                    updateConfig({expressions: newExprs});
                  },
                }),
                m('span', {style: {opacity: 0.5, fontSize: '11px'}}, 'as'),
                m(TextInput, {
                  placeholder: 'alias',
                  value: expr.alias,
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
                expressions: [...config.expressions, {expression: '', alias: ''}],
              });
            },
          }),
          makeDragBin(
            (idx) => updateConfig({expressions: config.expressions.filter((_, j) => j !== idx)}),
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
  render(config, updateConfig, ctx) {
    return m(SelectNodeContent, {config, updateConfig, ctx});
  },
  getOutputColumns,
  isValid,
  tryFold,
};
