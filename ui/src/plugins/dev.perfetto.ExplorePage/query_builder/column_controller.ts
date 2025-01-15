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
import {Intent} from '../../../widgets/common';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {EmptyState} from '../../../widgets/empty_state';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Checkbox} from '../../../widgets/checkbox';
import {SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';
import {TextInput} from '../../../widgets/text_input';

export class ColumnControllerRows {
  // The ID is used to indentify this option, and is used in callbacks.
  id: string;
  // Whether this column is selected or not.
  checked: boolean;
  // This is the name displayed and used for searching.
  column: SqlColumn;
  // What is the data source of the column. Used for formatting for SQL.
  source?: string;
  // Word column was renamed to.
  alias?: string;

  constructor(column: SqlColumn) {
    this.id = column.name;
    this.checked = true;

    this.column = column;
  }
}

export interface ColumnControllerDiff {
  id: string;
  checked: boolean;
  alias?: string;
}

export interface ColumnControllerAttrs {
  options: ColumnControllerRows[];
  onChange?: (diffs: ColumnControllerDiff[]) => void;
  fixedSize?: boolean;
  hasValidColumns: boolean;
}

export class ColumnController
  implements m.ClassComponent<ColumnControllerAttrs>
{
  view({attrs}: m.CVnode<ColumnControllerAttrs>) {
    const {options, fixedSize = true} = attrs;

    const filteredItems = options;

    return m(
      fixedSize
        ? '.pf-column-controller-panel.pf-column-controller-fixed-size'
        : '.pf-column-controller-panel',
      this.renderListOfItems(attrs, filteredItems),
    );
  }

  private renderListOfItems(
    attrs: ColumnControllerAttrs,
    options: ColumnControllerRows[],
  ) {
    const {onChange = () => {}} = attrs;
    const allChecked = options.every(({checked}) => checked);
    const anyChecked = options.some(({checked}) => checked);

    if (options.length === 0) {
      return m(EmptyState, {
        title: `No results.'`,
      });
    } else {
      return [
        m(
          '.pf-list',
          m(
            '.pf-column-controller-container',
            m(
              '.pf-column-controller-header',
              m('span', 'Options'),
              m(Button, {
                label: 'Select All',
                icon: Icons.SelectAll,
                compact: true,
                onclick: () => {
                  const diffs = options
                    .filter(({checked}) => !checked)
                    .map(({id, alias}) => ({id, checked: true, alias: alias}));
                  onChange(diffs);
                },
                disabled: allChecked,
              }),
              m(Button, {
                label: 'Clear All',
                icon: Icons.Deselect,
                compact: true,
                onclick: () => {
                  const diffs = options
                    .filter(({checked}) => checked)
                    .map(({id, alias}) => ({id, checked: false, alias: alias}));
                  onChange(diffs);
                },
                disabled: !anyChecked,
              }),
            ),
            this.renderColumnRows(attrs, options),
          ),
        ),
      ];
    }
  }

  private renderColumnRows(
    attrs: ColumnControllerAttrs,
    options: ColumnControllerRows[],
  ): m.Children {
    const {onChange = () => {}} = attrs;

    return options.map((item) => {
      const {id, checked, column, alias} = item;
      return m(
        '',
        {key: id},
        m(Checkbox, {
          label: column.name,
          checked,
          className: 'pf-column-controller-item',
          onchange: () => {
            onChange([{id, alias, checked: !checked}]);
          },
        }),
        ' as ',
        m(TextInput, {
          placeholder: item.alias ? item.alias : column.name,
          type: 'string',
          oninput: (e: KeyboardEvent) => {
            if (!e.target) return;
            onChange([
              {id, checked, alias: (e.target as HTMLInputElement).value.trim()},
            ]);
          },
        }),
        m(Popup, {
          className: 'pf-visible-on-hover',
          trigger: m(Button, {icon: 'info'}),
        }),
      );
    });
  }
}

export type PopupColumnControllerAttrs = ColumnControllerAttrs & {
  intent?: Intent;
  compact?: boolean;
  icon?: string;
  label: string;
  popupPosition?: PopupPosition;
};

// The same multi-select component that functions as a drop-down instead of
// a list.
export class PopupColumnController
  implements m.ClassComponent<PopupColumnControllerAttrs>
{
  view({attrs}: m.CVnode<PopupColumnControllerAttrs>) {
    const {icon, popupPosition = PopupPosition.Auto, intent, compact} = attrs;

    return m(
      Popup,
      {
        trigger: m(Button, {
          label: this.labelText(attrs),
          icon,
          intent,
          compact,
        }),
        position: popupPosition,
      },
      m(ColumnController, attrs as ColumnControllerAttrs),
    );
  }

  private labelText(attrs: PopupColumnControllerAttrs): string {
    const {label} = attrs;
    return label;
  }
}

export function hasDuplicateColumnsSelected(
  cols: ColumnControllerRows[],
): string[] {
  const seenNames: {[key: string]: boolean} = {};
  const duplicates: string[] = [];

  for (const col of cols) {
    const name = col.alias || col.column.name;
    if (seenNames[name] && col.checked) {
      duplicates.push(name);
    } else {
      seenNames[name] = true;
    }
  }

  return duplicates;
}