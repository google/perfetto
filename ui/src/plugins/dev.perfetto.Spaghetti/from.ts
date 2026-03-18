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
import {BaseNodeData} from './node_types';
import {Button, ButtonVariant} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Popup, PopupPosition} from '../../widgets/popup';
import {FuzzyFinder, FuzzySegment} from '../../base/fuzzy';

export interface FromNodeData extends BaseNodeData {
  readonly type: 'from';
  readonly table: string;
}

export function createFromNode(id: string, x: number, y: number): FromNodeData {
  return {type: 'from', id, x, y, table: 'slice'};
}

function renderFuzzySegments(segments: FuzzySegment[]): m.Children {
  return segments.map(({matching, value}) =>
    matching ? m('b', value) : value,
  );
}

function FromNodeContent(): m.Component<{
  node: FromNodeData;
  updateNode: (updates: Partial<Omit<FromNodeData, 'type' | 'id'>>) => void;
  tableNames: string[];
}> {
  let isOpen = false;
  let searchTerm = '';
  let selectedIndex = 0;

  return {
    view({attrs: {node, updateNode, tableNames}}) {
      const finder = new FuzzyFinder(tableNames, (t) => t);
      const results = finder.find(searchTerm);
      selectedIndex = Math.min(selectedIndex, Math.max(0, results.length - 1));

      const content = m(
        Popup,
        {
          trigger: m(Button, {
            variant: ButtonVariant.Filled,
            label: node.table || 'Pick table...',
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
                  updateNode({table: results[selectedIndex].item});
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
                          updateNode({table: result.item});
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

export function renderFromNode(
  node: FromNodeData,
  updateNode: (updates: Partial<Omit<FromNodeData, 'type' | 'id'>>) => void,
  tableNames: string[],
): m.Children {
  return m(FromNodeContent, {node, updateNode, tableNames});
}
