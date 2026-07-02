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
import type {
  ColumnContext,
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';

export interface ExtractArgEntry {
  readonly argName: string;
  readonly alias: string;
}

export interface ExtractArgConfig {
  readonly argSetIdCol: string;
  readonly extractions: ExtractArgEntry[];
}

function extractArgAlias(e: ExtractArgEntry): string {
  if (e.alias) return e.alias;
  if (e.argName) return e.argName.replace(/[^a-zA-Z0-9_]/g, '_');
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
        className: 'pf-qb-alias-btn',
        title: 'Add alias',
        onclick: () => {
          editing = true;
        },
      });
    },
  };
}

function ExtractArgContent(): m.Component<{
  config: ExtractArgConfig;
  updateConfig: (updates: Partial<ExtractArgConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m('.pf-qb-stack', [
        m('.pf-qb-section-label', 'Join Key'),
        m(ColumnPicker, {
          value: config.argSetIdCol,
          columns: ctx.availableColumns,
          placeholder: 'column',
          onSelect: (value: string) => {
            updateConfig({argSetIdCol: value});
          },
        }),
        m('.pf-qb-section-label', 'Args'),
        m('.pf-qb-filter-list', [
          ...config.extractions.map((entry, i) =>
            m(
              '.pf-qb-filter-row',
              {
                key: i,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', String(i));
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
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
                    const newExtractions = [...config.extractions];
                    const [moved] = newExtractions.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    newExtractions.splice(toIdx, 0, moved);
                    updateConfig({extractions: newExtractions});
                  }
                },
              },
              [
                m(Icon, {
                  icon: 'drag_indicator',
                  className: 'pf-qb-drag-handle',
                }),
                m(TextInput, {
                  placeholder: 'arg name',
                  value: entry.argName,
                  onChange: (value: string) => {
                    const newExtractions = [...config.extractions];
                    newExtractions[i] = {...entry, argName: value};
                    updateConfig({extractions: newExtractions});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: extractArgAlias(entry) || 'alias',
                  onChange: (value: string) => {
                    const newExtractions = [...config.extractions];
                    newExtractions[i] = {...entry, alias: value};
                    updateConfig({extractions: newExtractions});
                  },
                }),
                m(Button, {
                  icon: 'delete',
                  className: 'pf-qb-row-delete-inline',
                  title: 'Remove',
                  onclick: () => {
                    updateConfig({
                      extractions: config.extractions.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(Button, {
          label: 'Add extraction',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              extractions: [...config.extractions, {argName: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: ExtractArgConfig,
  updateConfig: (updates: Partial<ExtractArgConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(ExtractArgContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: ExtractArgConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const extractAliases: ColumnDef[] = config.extractions
    .filter((e) => e.argName)
    .map((e) => ({name: extractArgAlias(e)}));
  if (extractAliases.length > 0) {
    return [...(columns ?? []), ...extractAliases];
  }
  return columns;
}

function isValid(config: ExtractArgConfig): boolean {
  return config.extractions.every((e) => !e.argName || e.argName);
}

function tryFold(stmt: SqlStatement, config: ExtractArgConfig): boolean {
  // Can't append columns to a grouped query.
  if (stmt.groupBy) return false;
  const exprParts = config.extractions
    .filter((e) => e.argName)
    .map(
      (e) =>
        `extract_arg(${config.argSetIdCol}, '${e.argName}') AS ${extractArgAlias(e)}`,
    );
  if (exprParts.length > 0) {
    stmt.columns = [stmt.columns, ...exprParts].join(', ');
  }
  return true;
}

export const manifest: NodeManifest<ExtractArgConfig> = {
  title: 'Extract Args',
  icon: 'data_object',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 95,
  defaultConfig: () => ({argSetIdCol: '', extractions: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
