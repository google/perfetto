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
import type {ColumnDef} from '../graph_utils';

export interface ExtendEntry {
  readonly expression: string;
  readonly alias: string;
}

export interface ExtendConfig {
  readonly entries: ExtendEntry[];
}

function entryAlias(e: ExtendEntry): string {
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
        className: 'pf-qb-alias-btn',
        title: 'Add alias',
        onclick: () => {
          editing = true;
        },
      });
    },
  };
}

function ExtendContent(): m.Component<{
  config: ExtendConfig;
  updateConfig: (updates: Partial<ExtendConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig}}) {
      return m('.pf-qb-stack', [
        m('.pf-qb-filter-list', [
          ...config.entries.map((entry, i) =>
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
                  const fromIdx = parseInt(e.dataTransfer!.getData('text/plain'));
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...config.entries];
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
                m(TextInput, {
                  placeholder: 'expression',
                  value: entry.expression,
                  onChange: (value: string) => {
                    const updated = [...config.entries];
                    updated[i] = {...entry, expression: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: entryAlias(entry) || 'alias',
                  onChange: (value: string) => {
                    const updated = [...config.entries];
                    updated[i] = {...entry, alias: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(Button, {
                  icon: 'delete',
                  className: 'pf-qb-row-delete-inline',
                  title: 'Remove',
                  onclick: () => {
                    updateConfig({
                      entries: config.entries.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(Button, {
          label: 'Add column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              entries: [...config.entries, {expression: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: ExtendConfig,
  updateConfig: (updates: Partial<ExtendConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(ExtendContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: ExtendConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const added: ColumnDef[] = config.entries
    .filter((e) => e.expression)
    .map((e) => ({name: entryAlias(e)}));
  if (added.length > 0) {
    return [...(columns ?? []), ...added];
  }
  return columns;
}

function isValid(config: ExtendConfig): boolean {
  return config.entries.every((e) => !e.alias || e.expression);
}

function tryFold(stmt: SqlStatement, config: ExtendConfig): boolean {
  // Can't append columns to a grouped query.
  if (stmt.groupBy) return false;
  const exprParts = config.entries
    .filter((e) => e.expression)
    .map((e) => `${e.expression} AS ${entryAlias(e)}`);
  if (exprParts.length > 0) {
    stmt.columns = [stmt.columns, ...exprParts].join(', ');
  }
  return true;
}

export const manifest: NodeManifest<ExtendConfig> = {
  title: 'Extend',
  icon: 'add_column_right',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 125,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
