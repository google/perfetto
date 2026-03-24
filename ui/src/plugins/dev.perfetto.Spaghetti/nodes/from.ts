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
import {NodeManifest, RenderContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {TextInput} from '../../../widgets/text_input';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {FuzzyFinder, FuzzySegment} from '../../../base/fuzzy';

export interface FromConfig {
  readonly table: string;
}

function renderFuzzySegments(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('b', value) : value,
  );
}

function FromNodeContent(): m.Component<{
  config: FromConfig;
  updateConfig: (updates: Partial<FromConfig>) => void;
  tableNames: string[];
}> {
  let isOpen = false;
  let searchTerm = '';
  let selectedIndex = 0;

  return {
    view({attrs: {config, updateConfig, tableNames}}) {
      const finder = new FuzzyFinder(tableNames, (t) => t);
      const results = finder.find(searchTerm);
      selectedIndex = Math.min(selectedIndex, Math.max(0, results.length - 1));

      const content = m(
        Popup,
        {
          trigger: m(Button, {
            variant: ButtonVariant.Filled,
            label: config.table || 'Pick table...',
            icon: 'table_chart',
            onclick: () => {
              isOpen = !isOpen;
            },
          }),
          isOpen,
          onChange: (shouldOpen) => {
            isOpen = shouldOpen;
            if (!shouldOpen) {
              searchTerm = '';
              selectedIndex = 0;
            }
          },
          position: PopupPosition.Bottom,
          closeOnEscape: true,
          closeOnOutsideClick: true,
          showArrow: false,
        },
        m(
          '.pf-from-node-picker.pf-qb-stack',
          {
            style: {
              maxHeight: '300px',
              minWidth: '200px',
            },
          },
          [
            m(TextInput, {
              leftIcon: 'search',
              placeholder: 'Search tables...',
              value: searchTerm,
              autofocus: true,
              oninput: (e: InputEvent) => {
                searchTerm = (e.target as HTMLInputElement).value;
                selectedIndex = 0;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'ArrowDown') {
                  selectedIndex = Math.min(
                    selectedIndex + 1,
                    results.length - 1,
                  );
                  e.preventDefault();
                } else if (e.key === 'ArrowUp') {
                  selectedIndex = Math.max(selectedIndex - 1, 0);
                  e.preventDefault();
                } else if (e.key === 'Enter' && results.length > 0) {
                  updateConfig({table: results[selectedIndex].item});
                  isOpen = false;
                  searchTerm = '';
                  selectedIndex = 0;
                  e.preventDefault();
                }
              },
            }),
            m(
              '.pf-from-node-results',
              {
                style: {
                  overflowY: 'auto',
                  flex: '1',
                },
              },
              results.length === 0
                ? m(
                    'div',
                    {style: {padding: '8px', opacity: '0.5'}},
                    'No matches',
                  )
                : results.map((result, i) =>
                    m(
                      '.pf-from-node-result',
                      {
                        style: {
                          padding: '4px 8px',
                          cursor: 'pointer',
                          background:
                            i === selectedIndex
                              ? 'var(--hover-background)'
                              : 'transparent',
                        },
                        onmouseenter: () => {
                          selectedIndex = i;
                        },
                        onclick: () => {
                          updateConfig({table: result.item});
                          isOpen = false;
                          searchTerm = '';
                          selectedIndex = 0;
                        },
                      },
                      renderFuzzySegments(result.segments),
                    ),
                  ),
            ),
          ],
        ),
      );

      return m('.pf-qb-stack', content);
    },
  };
}

export const manifest: NodeManifest<FromConfig> = {
  title: 'From',
  icon: 'table_chart',
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockBottom: true,
  hue: 210,
  defaultConfig: () => ({table: 'slice'}),
  isValid: (config) => config.table !== '',
  emitIr: (config) => ({sql: `SELECT *\nFROM ${config.table}`}),
  getOutputColumns(config, ctx) {
    if (!config.table || !ctx.sqlModules) return undefined;
    const table = ctx.sqlModules.getTable(config.table);
    return table
      ? table.columns.map((c) => ({name: c.name, type: c.type}))
      : undefined;
  },
  render(
    config: FromConfig,
    updateConfig: (updates: Partial<FromConfig>) => void,
    ctx: RenderContext,
  ): m.Children {
    return m(FromNodeContent, {
      config,
      updateConfig,
      tableNames: ctx.tableNames,
    });
  },
};
