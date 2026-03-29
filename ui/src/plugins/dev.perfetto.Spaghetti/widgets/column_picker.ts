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
import {Popup, PopupPosition} from '../../../widgets/popup';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {perfettoSqlTypeIcon} from '../../../trace_processor/perfetto_sql_type';
import {ColumnDef} from '../graph_utils';
import {classNames} from '../../../base/classnames';

// A text input with an autocomplete dropdown for column names.
//
// UX behavior (URL-bar style):
// - Click/focus: select all text, open dropdown showing ALL columns.
// - Type (replaces selection): dropdown filters to matches.
// - Click around / backspace: continues filtering on current text.
// - Arrow keys + Enter: navigate and select from list.
// - A "Use 'XYZ'" option appears for free-form values not in the list.

export interface ColumnPickerAttrs {
  readonly value: string;
  readonly columns: ColumnDef[];
  readonly onSelect: (value: string) => void;
  readonly placeholder?: string;
  readonly className?: string;
}

export class ColumnPicker implements m.ClassComponent<ColumnPickerAttrs> {
  private isOpen = false;
  private query = '';
  private editing = false;
  // Tracks whether the user has modified text since focus.
  // Before any edit, show all columns unfiltered.
  private hasTyped = false;
  private highlightIdx = 0;

  private getFiltered(columns: ColumnDef[]): ColumnDef[] {
    if (!this.hasTyped || this.query === '') return columns;
    const q = this.query.toLowerCase();
    return columns.filter((c) => c.name.toLowerCase().includes(q));
  }

  private commit(value: string, attrs: ColumnPickerAttrs) {
    this.isOpen = false;
    this.editing = false;
    this.hasTyped = false;
    this.query = '';
    attrs.onSelect(value);
  }

  private reset() {
    this.isOpen = false;
    this.editing = false;
    this.hasTyped = false;
    this.query = '';
  }

  view({attrs}: m.CVnode<ColumnPickerAttrs>) {
    const displayValue = this.editing ? this.query : attrs.value;
    const filtered = this.getFiltered(attrs.columns);
    const exactMatch =
      this.hasTyped &&
      filtered.some((c) => c.name.toLowerCase() === this.query.toLowerCase());
    const showAddOption = this.hasTyped && this.query.length > 0 && !exactMatch;
    const hasItems = filtered.length > 0 || showAddOption;
    const isUnknown =
      attrs.value !== '' &&
      attrs.columns.length > 0 &&
      !attrs.columns.some((c) => c.name === attrs.value);

    return m(
      Popup,
      {
        trigger: m(TextInput, {
          value: displayValue,
          placeholder: attrs.placeholder ?? 'column',
          className: classNames(
            isUnknown && 'pf-column-picker--unknown',
            attrs.className,
          ),
          onfocus: (e: FocusEvent) => {
            this.editing = true;
            this.query = attrs.value;
            this.hasTyped = false;
            this.highlightIdx = 0;
            this.isOpen = true;
            const input = e.target as HTMLInputElement;
            requestAnimationFrame(() => input.select());
          },
          onInput: (value: string) => {
            this.query = value;
            this.hasTyped = true;
            this.highlightIdx = 0;
            this.isOpen = true;
          },
          onkeydown: (e: KeyboardEvent) => {
            const itemCount = filtered.length + (showAddOption ? 1 : 0);
            if (e.key === 'ArrowDown') {
              e.preventDefault();
              this.highlightIdx = Math.min(
                this.highlightIdx + 1,
                itemCount - 1,
              );
            } else if (e.key === 'ArrowUp') {
              e.preventDefault();
              this.highlightIdx = Math.max(this.highlightIdx - 1, 0);
            } else if (e.key === 'Enter') {
              e.preventDefault();
              if (itemCount === 0) {
                if (this.query) this.commit(this.query, attrs);
              } else if (this.highlightIdx < filtered.length) {
                this.commit(filtered[this.highlightIdx].name, attrs);
              } else if (showAddOption) {
                this.commit(this.query, attrs);
              }
              (e.target as HTMLElement).blur();
            } else if (e.key === 'Escape') {
              this.reset();
              (e.target as HTMLElement).blur();
            }
          },
        }),
        isOpen: this.isOpen && hasItems,
        onChange: (shouldOpen: boolean) => {
          if (!shouldOpen) {
            if (this.editing && this.query) {
              attrs.onSelect(this.query);
            }
            this.reset();
          }
        },
        position: PopupPosition.Bottom,
        closeOnEscape: true,
        closeOnOutsideClick: true,
        className: `pf-column-picker__popup${attrs.className ? ` ${attrs.className}` : ''}`,
      },
      m('.pf-column-picker__list', [
        ...filtered.map((col, idx) =>
          m(
            '.pf-column-picker__item',
            {
              class: idx === this.highlightIdx ? 'highlight' : '',
              onmousedown: (e: MouseEvent) => {
                e.preventDefault();
                this.commit(col.name, attrs);
              },
              onmouseenter: () => {
                this.highlightIdx = idx;
              },
            },
            [
              col.type !== undefined &&
                m(Icon, {
                  icon: perfettoSqlTypeIcon(col.type),
                  className: 'pf-column-picker__type-icon',
                }),
              col.name,
            ],
          ),
        ),
        showAddOption &&
          m(
            '.pf-column-picker__item.pf-column-picker__add',
            {
              class: this.highlightIdx === filtered.length ? 'highlight' : '',
              onmousedown: (e: MouseEvent) => {
                e.preventDefault();
                this.commit(this.query, attrs);
              },
              onmouseenter: () => {
                this.highlightIdx = filtered.length;
              },
            },
            `Use '${this.query}'`,
          ),
      ]),
    );
  }
}
