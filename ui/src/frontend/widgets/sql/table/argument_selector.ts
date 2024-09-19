// Copyright (C) 2024 The Android Open Source Project
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
import {raf} from '../../../../core/raf_scheduler';
import {Spinner} from '../../../../widgets/spinner';
import {
  TableColumn,
  tableColumnId,
  TableColumnSet,
  TableManager,
} from './column';
import {TextInput} from '../../../../widgets/text_input';
import {scheduleFullRedraw} from '../../../../widgets/raf';
import {hasModKey, modKey} from '../../../../base/hotkeys';
import {MenuItem} from '../../../../widgets/menu';
import {uuidv4} from '../../../../base/uuid';

const MAX_ARGS_TO_DISPLAY = 15;

interface ArgumentSelectorAttrs {
  tableManager: TableManager;
  columnSet: TableColumnSet;
  alreadySelectedColumnIds: Set<string>;
  onArgumentSelected: (column: TableColumn) => void;
}

// This class is responsible for rendering a menu which allows user to select which column out of ColumnSet to add.
export class ArgumentSelector
  implements m.ClassComponent<ArgumentSelectorAttrs>
{
  searchText = '';
  columns?: {key: string; column: TableColumn | TableColumnSet}[];

  constructor({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    this.load(attrs);
  }

  private async load(attrs: ArgumentSelectorAttrs) {
    this.columns = await attrs.columnSet.discover(attrs.tableManager);
    raf.scheduleFullRedraw();
  }

  view({attrs}: m.Vnode<ArgumentSelectorAttrs>) {
    const columns = this.columns;
    if (columns === undefined) return m(Spinner);

    // Candidates are the columns which have not been selected yet.
    const candidates = columns.filter(
      ({column}) =>
        column instanceof TableColumnSet ||
        !attrs.alreadySelectedColumnIds.has(tableColumnId(column)),
    );

    // Filter the candidates based on the search text.
    const filtered = candidates.filter(({key}) => {
      return key.toLowerCase().includes(this.searchText.toLowerCase());
    });

    const displayed = filtered.slice(0, MAX_ARGS_TO_DISPLAY);

    const extraItems = Math.max(0, filtered.length - MAX_ARGS_TO_DISPLAY);

    const firstButtonUuid = uuidv4();

    return [
      m(
        '.pf-search-bar',
        m(TextInput, {
          autofocus: true,
          oninput: (event: Event) => {
            const eventTarget = event.target as HTMLTextAreaElement;
            this.searchText = eventTarget.value;
            scheduleFullRedraw();
          },
          onkeydown: (event: KeyboardEvent) => {
            if (filtered.length === 0) return;
            if (event.key === 'Enter') {
              // If there is only one item or Mod-Enter was pressed, select the first element.
              if (filtered.length === 1 || hasModKey(event)) {
                const params = {bubbles: true};
                if (hasModKey(event)) {
                  Object.assign(params, modKey());
                }
                const pointerEvent = new PointerEvent('click', params);
                (
                  document.getElementById(firstButtonUuid) as HTMLElement | null
                )?.dispatchEvent(pointerEvent);
              }
            }
          },
          value: this.searchText,
          placeholder: 'Filter...',
          className: 'pf-search-box',
        }),
      ),
      ...displayed.map(({key, column}, index) =>
        m(
          MenuItem,
          {
            id: index === 0 ? firstButtonUuid : undefined,
            label: key,
            onclick: (event) => {
              if (column instanceof TableColumnSet) return;
              attrs.onArgumentSelected(column);
              // For Control-Click, we don't want to close the menu to allow the user
              // to select multiple items in one go.
              if (hasModKey(event)) {
                event.stopPropagation();
              }
              // Otherwise this popup will be closed.
            },
          },
          column instanceof TableColumnSet &&
            m(ArgumentSelector, {
              columnSet: column,
              alreadySelectedColumnIds: attrs.alreadySelectedColumnIds,
              onArgumentSelected: attrs.onArgumentSelected,
              tableManager: attrs.tableManager,
            }),
        ),
      ),
      Boolean(extraItems) && m('i', `+${extraItems} more`),
    ];
  }
}
