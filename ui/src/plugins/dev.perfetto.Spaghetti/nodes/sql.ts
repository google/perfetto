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
import {Button, ButtonVariant} from '../../../widgets/button';
import {TextInput} from '../../../widgets/text_input';
import {Select} from '../../../widgets/select';
import type {NodeManifest, RenderContext} from '../node_types';
import type {ColumnDef} from '../graph_utils';
import type {SimpleTypeKind} from '../../../trace_processor/perfetto_sql_type';
import type {Port} from '../graph_model';

export interface SqlOutputColumn {
  readonly name: string;
  readonly type: string; // SimpleTypeKind or '' for unknown
}

export interface SqlConfig {
  readonly sql: string;
  readonly columns?: SqlOutputColumn[];
  /** User-defined alias for each input port, parallel to the inputs array. */
  readonly inputPorts?: string[];
  /** Persisted textarea dimensions. Undefined = use defaults. */
  readonly textareaWidth?: number;
  readonly textareaHeight?: number;
}

const DEFAULT_TEXTAREA_WIDTH = 220;
const DEFAULT_TEXTAREA_HEIGHT = 120;

const TYPE_OPTIONS: {label: string; value: string}[] = [
  {label: '(unknown)', value: ''},
  {label: 'int', value: 'int'},
  {label: 'double', value: 'double'},
  {label: 'string', value: 'string'},
  {label: 'boolean', value: 'boolean'},
  {label: 'timestamp', value: 'timestamp'},
  {label: 'duration', value: 'duration'},
  {label: 'bytes', value: 'bytes'},
];

function toColumnDef(col: SqlOutputColumn): ColumnDef {
  if (!col.type) return {name: col.name};
  return {name: col.name, type: {kind: col.type as SimpleTypeKind}};
}

export const manifest: NodeManifest<SqlConfig> = {
  title: 'SQL',
  icon: 'code',
  hue: 280,
  getInputs(config: SqlConfig): Port[] {
    const {inputPorts = []} = config;
    return inputPorts.map((name, i) => ({
      name: `input_${i}`,
      content: name,
    }));
  },
  defaultConfig: () => ({
    sql: 'SELECT\n  *\nFROM slice\nLIMIT 100',
    columns: [],
    inputPorts: [],
  }),
  isValid: (config) => config.sql.trim() !== '',
  emitIr(config, ctx) {
    const {inputPorts = []} = config;
    if (config.sql.trim() === '') return undefined;
    const withClauses: string[] = [];
    for (let i = 0; i < ctx.inputPorts.length; i++) {
      const port = ctx.inputPorts[i];
      const ref = ctx.getInputRef(port.name);
      if (!ref) continue;
      const alias = inputPorts[i]?.trim() || `input_${i}`;
      withClauses.push(`${alias} AS (\n  SELECT * FROM ${ref}\n)`);
    }
    if (withClauses.length === 0) {
      return {sql: `${config.sql}`};
    }
    const trimmed = config.sql.trimStart();
    const withMatch = trimmed.match(/^with\s/i);
    const body = withMatch ? trimmed.slice(withMatch[0].length) : trimmed;
    const separator = withMatch ? ',\n' : '\n';
    return {
      sql: `WITH ${withClauses.join(',\n')}${separator}${body}`,
    };
  },
  getOutputColumns(config) {
    const {columns = []} = config;
    if (columns.length === 0) return undefined;
    return columns.map(toColumnDef);
  },
  render(config, updateConfig, ctx: RenderContext) {
    const {columns = [], inputPorts = []} = config;
    const canRemove = inputPorts.length > 0;

    return m('.pf-qb-stack', [
      // Input table aliases
      inputPorts.length > 0 &&
        m('.pf-qb-stack', [
          m('span.pf-qb-section-label', 'Input tables'),
          m(
            'div',
            {style: {display: 'flex', flexDirection: 'column', gap: '2px'}},
            ctx.inputPorts.map((port, i) =>
              m('.pf-qb-sql-col-row', {key: port.name}, [
                m(
                  'span.pf-qb-hint',
                  {style: {gridColumn: 'span 1'}},
                  `#${i + 1}`,
                ),
                m(TextInput, {
                  placeholder: `alias (e.g. events)`,
                  value: inputPorts[i] ?? '',
                  onInput: (value: string) => {
                    const next = [...inputPorts];
                    next[i] = value;
                    updateConfig({inputPorts: next});
                  },
                }),
              ]),
            ),
          ),
        ]),
      // Add / remove input buttons
      m('div', {style: {display: 'flex', gap: '4px'}}, [
        canRemove &&
          m(Button, {
            label: '- Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: () =>
              updateConfig({
                inputPorts: inputPorts.slice(0, inputPorts.length - 1),
              }),
          }),
        m(Button, {
          label: '+ Input',
          variant: ButtonVariant.Filled,
          style: {flex: '1'},
          onclick: () =>
            updateConfig({
              inputPorts: [...inputPorts, `input_${inputPorts.length}`],
            }),
        }),
      ]),
      // SQL body
      m('span.pf-qb-section-label', 'SQL'),
      m('textarea.pf-qb-sql-textarea', {
        value: config.sql,
        spellcheck: false,
        style: {
          width: `${config.textareaWidth ?? DEFAULT_TEXTAREA_WIDTH}px`,
          height: `${config.textareaHeight ?? DEFAULT_TEXTAREA_HEIGHT}px`,
        },
        oninput: (e: InputEvent) => {
          updateConfig({sql: (e.target as HTMLTextAreaElement).value});
        },
        onmouseup: (e: MouseEvent) => {
          const el = e.target as HTMLTextAreaElement;
          updateConfig({
            textareaWidth: el.offsetWidth,
            textareaHeight: el.offsetHeight,
          });
        },
      }),
      // Output column declarations
      m('span.pf-qb-section-label', 'Output columns'),
      columns.length > 0 &&
        m(
          'div',
          {style: {display: 'flex', flexDirection: 'column', gap: '2px'}},
          columns.map((col, i) =>
            m('.pf-qb-sql-col-row', {key: i}, [
              m(TextInput, {
                placeholder: 'column name',
                value: col.name,
                onChange: (value: string) => {
                  const next = [...columns];
                  next[i] = {...col, name: value};
                  updateConfig({columns: next});
                },
              }),
              m(
                Select,
                {
                  value: col.type,
                  onchange: (e: Event) => {
                    const next = [...columns];
                    next[i] = {
                      ...col,
                      type: (e.target as HTMLSelectElement).value,
                    };
                    updateConfig({columns: next});
                  },
                },
                TYPE_OPTIONS.map((opt) =>
                  m('option', {value: opt.value}, opt.label),
                ),
              ),
              m(Button, {
                icon: 'close',
                variant: ButtonVariant.Minimal,
                onclick: () => {
                  updateConfig({columns: columns.filter((_, j) => j !== i)});
                },
              }),
            ]),
          ),
        ),
      m(Button, {
        label: 'Add output column',
        icon: 'add',
        variant: ButtonVariant.Filled,
        onclick: () => {
          updateConfig({columns: [...columns, {name: '', type: ''}]});
        },
      }),
    ]);
  },
};
