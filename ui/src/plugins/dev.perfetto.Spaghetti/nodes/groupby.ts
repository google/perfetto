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
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import {ColumnDef} from '../graph_utils';

export interface Aggregation {
  readonly func: 'COUNT' | 'SUM' | 'AVG' | 'MIN' | 'MAX';
  readonly column: string;
  readonly alias: string;
}

export interface GroupByConfig {
  readonly groupColumns: string[];
  readonly aggregations: Aggregation[];
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
        icon: 'label',
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

function aggAlias(a: Aggregation): string {
  if (a.alias) return a.alias;
  const col = a.column === '*' ? 'star' : a.column;
  return `${a.func.toLowerCase()}_${col}`;
}

function GroupByContent(): m.Component<{
  config: GroupByConfig;
  updateConfig: (updates: Partial<GroupByConfig>) => void;
  ctx: RenderContext;
}> {
  let draggingGroup = false;
  let draggingAgg = false;
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
        draggingGroup = false;
        draggingAgg = false;
        const data = e.dataTransfer!.getData('text/plain');
        const idx = parseInt(data.includes(':') ? data.split(':')[1] : data);
        onDrop(idx);
      },
    }, m(Icon, {icon: 'delete'}));
  }

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m('.pf-qb-stack', {style: {minWidth: '250px'}}, [
        m('.pf-qb-section-label', 'Group by'),
        m('.pf-qb-filter-list', [
          ...config.groupColumns.map((col, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: i,
                draggable: config.groupColumns.length > 1,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `group:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingGroup = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
                  draggingGroup = false;
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
                  if (!data.startsWith('group:')) return;
                  const fromIdx = parseInt(data.slice(6));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...config.groupColumns];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({groupColumns: updated});
                  }
                },
              },
              [
                ...(config.groupColumns.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
                m(ColumnPicker, {
                  value: col,
                  columns: ctx.availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...config.groupColumns];
                    updated[i] = value;
                    updateConfig({groupColumns: updated});
                  },
                }),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Grouping',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () => {
              updateConfig({groupColumns: [...config.groupColumns, '']});
            },
          }),
          makeDragBin(
            (idx) => updateConfig({groupColumns: config.groupColumns.filter((_, j) => j !== idx)}),
            draggingGroup,
          ),
        ]),

        m('.pf-qb-section-label', 'Aggregations'),
        m('.pf-qb-filter-list', [
          ...config.aggregations.map((agg, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: i,
                draggable: config.aggregations.length > 1,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', `agg:${i}`);
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  draggingAgg = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
                  draggingAgg = false;
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
                  if (!data.startsWith('agg:')) return;
                  const fromIdx = parseInt(data.slice(4));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const newAggs = [...config.aggregations];
                    const [moved] = newAggs.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    newAggs.splice(toIdx, 0, moved);
                    updateConfig({aggregations: newAggs});
                  }
                },
              },
              [
                ...(config.aggregations.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
                m(
                  Select,
                  {
                    value: agg.func,
                    onchange: (e: Event) => {
                      const newAggs = [...config.aggregations];
                      newAggs[i] = {
                        ...agg,
                        func: (e.target as HTMLSelectElement)
                          .value as Aggregation['func'],
                      };
                      updateConfig({aggregations: newAggs});
                    },
                  },
                  [
                    m('option', {value: 'COUNT'}, 'COUNT'),
                    m('option', {value: 'SUM'}, 'SUM'),
                    m('option', {value: 'AVG'}, 'AVG'),
                    m('option', {value: 'MIN'}, 'MIN'),
                    m('option', {value: 'MAX'}, 'MAX'),
                  ],
                ),
                m(ColumnPicker, {
                  value: agg.column,
                  columns: [{name: '*'}, ...ctx.availableColumns],
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const newAggs = [...config.aggregations];
                    newAggs[i] = {...agg, column: value};
                    updateConfig({aggregations: newAggs});
                  },
                }),
                m(AliasTag, {
                  alias: agg.alias,
                  placeholder: aggAlias(agg),
                  onChange: (value: string) => {
                    const newAggs = [...config.aggregations];
                    newAggs[i] = {...agg, alias: value};
                    updateConfig({aggregations: newAggs});
                  },
                }),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Aggregation',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () => {
              updateConfig({
                aggregations: [
                  ...config.aggregations,
                  {func: 'COUNT', column: '*', alias: ''},
                ],
              });
            },
          }),
          makeDragBin(
            (idx) => updateConfig({aggregations: config.aggregations.filter((_, j) => j !== idx)}),
            draggingAgg,
          ),
        ]),
      ]);
    },
  };
}

function render(
  config: GroupByConfig,
  updateConfig: (updates: Partial<GroupByConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(GroupByContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: GroupByConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const groupCols: ColumnDef[] = config.groupColumns
    .filter((c) => c)
    .map((c) => columns?.find((col) => col.name === c) ?? {name: c});
  const aggAliases: ColumnDef[] = config.aggregations
    .filter((a) => a.column)
    .map((a) => {
      const name = aggAlias(a);
      if (a.func === 'COUNT') {
        return {name, type: {kind: 'int' as const}};
      }
      const orig = columns?.find((c) => c.name === a.column);
      return {name, type: orig?.type};
    });
  const result = [...groupCols, ...aggAliases];
  return result.length > 0 ? result : columns;
}

function isValid(config: GroupByConfig): boolean {
  const hasGroup = config.groupColumns.some((c) => c);
  return hasGroup;
}

function tryFold(stmt: SqlStatement, config: GroupByConfig): boolean {
  if (
    stmt.columns !== '*' ||
    stmt.groupBy !== undefined ||
    stmt.orderBy !== undefined ||
    stmt.limit !== undefined
  ) {
    return false;
  }
  const groupCols = config.groupColumns.filter((c) => c);
  const aggExprs = config.aggregations
    .filter((a) => a.column)
    .map((a) => `${a.func}(${a.column}) AS ${aggAlias(a)}`);
  const selectParts = [...groupCols, ...aggExprs];
  if (selectParts.length > 0) stmt.columns = selectParts.join(', ');
  if (groupCols.length > 0) stmt.groupBy = groupCols.join(', ');
  return true;
}

export const manifest: NodeManifest<GroupByConfig> = {
  title: 'Group By',
  icon: 'workspaces',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 275,
  defaultConfig: () => ({groupColumns: [], aggregations: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
