// Copyright (C) 2026 The Android Open Source Project
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
import {NodeManifest, RenderContext, IrContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';

export interface JoinColumn {
  readonly column: string;
  readonly alias: string;
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
        className: 'pf-qb-alias-btn',
        title: 'Add alias',
        onclick: () => {
          editing = true;
        },
      });
    },
  };
}

export interface JoinConfig {
  readonly leftColumn: string;
  readonly rightColumn: string;
  readonly columns: JoinColumn[];
  readonly joinType?: 'LEFT' | 'INNER';
}

// Compute the resolved aliases for all extend columns.
export function getExtendColumnAliases(
  config: JoinConfig,
  leftAvail: string[],
): JoinColumn[] {
  const leftSet = new Set(leftAvail);
  return config.columns.map((c) => ({
    column: c.column,
    alias: c.alias || (leftSet.has(c.column) ? `right_${c.column}` : c.column),
  }));
}

function JoinNodeContent(): m.Component<{
  config: JoinConfig;
  updateConfig: (updates: Partial<JoinConfig>) => void;
  ctx: RenderContext;
}> {
  let dragging = false;
  let binHover = false;

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const leftColumns = ctx.getInputColumns('left');
      const rightColumns = ctx.getInputColumns('right');
      const leftSet = new Set(leftColumns.map((c) => c.name));

      const joinType = config.joinType ?? 'LEFT';
      return m('.pf-qb-stack', {style: {minWidth: '200px'}}, [
        m(SegmentedButtons, {
          fillWidth: true,
          options: [{label: 'Left'}, {label: 'Inner'}],
          selectedOption: joinType === 'LEFT' ? 0 : 1,
          onOptionSelected: (i) =>
            updateConfig({joinType: i === 0 ? 'LEFT' : 'INNER'}),
        }),
        m('.pf-qb-section-label', 'Join on'),
        m('.pf-qb-group-grid', [
          m(ColumnPicker, {
            value: config.leftColumn,
            columns: leftColumns,
            placeholder: 'left col',
            className: 'pf-qb-join-col',
            onSelect: (value: string) => {
              updateConfig({leftColumn: value});
            },
          }),
          m('span', {style: {opacity: 0.5}}, '='),
          m(ColumnPicker, {
            value: config.rightColumn,
            columns: rightColumns,
            placeholder: 'right col',
            className: 'pf-qb-join-col',
            onSelect: (value: string) => {
              updateConfig({rightColumn: value});
            },
          }),
        ]),

        m('.pf-qb-section-label', 'Columns to add'),
        m('.pf-qb-filter-list', [
          ...config.columns.map((entry, i) => {
            const defaultAlias = leftSet.has(entry.column)
              ? `right_${entry.column}`
              : entry.column;
            return m(
              '.pf-qb-filter-row',
              {
                key: i,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', String(i));
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                  dragging = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                  dragging = false;
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
                  const fromIdx = parseInt(
                    e.dataTransfer!.getData('text/plain'),
                  );
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...config.columns];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({columns: updated});
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
                  columns: rightColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...config.columns];
                    updated[i] = {...entry, column: value};
                    updateConfig({columns: updated});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: defaultAlias,
                  onChange: (value: string) => {
                    const updated = [...config.columns];
                    updated[i] = {...entry, alias: value};
                    updateConfig({columns: updated});
                  },
                }),
              ],
            );
          }),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Column',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () => {
              updateConfig({
                columns: [...config.columns, {column: '', alias: ''}],
              });
            },
          }),
          dragging
            ? m(
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
                    dragging = false;
                    const fromIdx = parseInt(
                      e.dataTransfer!.getData('text/plain'),
                    );
                    updateConfig({
                      columns: config.columns.filter((_, j) => j !== fromIdx),
                    });
                  },
                },
                m(Icon, {icon: 'delete'}),
              )
            : null,
        ]),
      ]);
    },
  };
}

function render(
  config: JoinConfig,
  updateConfig: (updates: Partial<JoinConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(JoinNodeContent, {config, updateConfig, ctx});
}

function isValid(config: JoinConfig): boolean {
  return config.leftColumn !== '' && config.rightColumn !== '';
}

export const manifest: NodeManifest<JoinConfig> = {
  title: 'Join',
  icon: 'add_circle',
  inputs: [
    {name: 'left', content: 'Left', direction: 'left'},
    {name: 'right', content: 'Right', direction: 'left'},
  ],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 308,
  getOutputColumns(config, ctx) {
    // Output columns are all left columns, plus select right columns.
    const leftCols = ctx.getInputColumns('left') ?? [];
    const rightCols = ctx.getInputColumns('right') ?? [];
    const leftSet = new Set(leftCols.map((c) => c.name));
    const outputCols = [...leftCols];

    for (const entry of config.columns) {
      const colName = entry.column;
      if (!colName) continue;
      const rightCol = rightCols.find((c) => c.name === colName);
      if (rightCol) {
        const alias =
          entry.alias || (leftSet.has(colName) ? `right_${colName}` : colName);
        outputCols.push({name: alias, type: rightCol.type});
      }
    }

    return outputCols;
  },
  defaultConfig: () => ({leftColumn: '', rightColumn: '', columns: []}),
  render,
  isValid,
  emitIr(config: JoinConfig, ctx: IrContext) {
    const leftRef = ctx.getInputRef('left');
    const rightRef = ctx.getInputRef('right');
    if (!leftRef) return undefined;
    if (!rightRef) return {sql: `SELECT *\nFROM ${leftRef}`};
    const leftCols = ctx.getInputColumns('left');

    const selectCols: string[] = ['l.*'];
    const aliases = getExtendColumnAliases(
      config,
      (leftCols ?? []).map((c) => c.name),
    );
    for (const a of aliases) {
      const expr = `r.${a.column}`;
      selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
    }
    let selectClause: string;
    if (selectCols.length > 1) {
      selectClause = '\n  ' + selectCols.join(',\n  ');
    } else {
      selectClause = selectCols[0];
    }
    const joinKw =
      (config.joinType ?? 'LEFT') === 'INNER' ? 'JOIN' : 'LEFT JOIN';
    const condition = `ON l.${config.leftColumn} = r.${config.rightColumn}`;
    const sql = `SELECT ${selectClause}\nFROM ${leftRef} AS l\n${joinKw} ${rightRef} AS r ${condition}`;
    return {sql};
  },
};
