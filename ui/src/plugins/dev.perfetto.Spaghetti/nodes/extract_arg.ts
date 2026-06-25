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
import {TextInput} from '../../../widgets/text_input';
import {moveItem, Row} from '../components/row';
import {AddButton} from '../components/add_button';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';
import {AliasTag} from '../components/alias_tag';

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

function ExtractArgContent(): m.Component<{
  config: ExtractArgConfig;
  updateConfig: (updates: Partial<ExtractArgConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, [
        m('.pf-spag-section-label', 'Join Key'),
        m(ColumnPicker, {
          value: config.argSetIdCol,
          columns: ctx.availableColumns,
          placeholder: 'column',
          onSelect: (value: string) => {
            updateConfig({argSetIdCol: value});
          },
        }),
        m('.pf-spag-section-label', 'Args'),
        m(Stack, {compact: true}, [
          ...config.extractions.map((entry, i) =>
            m(
              Row,
              {
                key: i,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({
                      extractions: moveItem(config.extractions, from, to),
                    });
                  },
                },
              },
              [
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
                m(Row.DeleteButton, {
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
        m(AddButton, {
          label: 'Add arg',
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
  return config.extractions.every((e) => !e.alias || !!e.argName);
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
