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
import {Chip} from '../../../widgets/chip';
import {Icon} from '../../../widgets/icon';
import {Menu, MenuDivider, MenuItem} from '../../../widgets/menu';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {TextInput} from '../../../widgets/text_input';
import './process_header.scss';

export interface ProcessOption {
  readonly key: string;
  readonly name: string;
  readonly pid: number;
  // Secondary text in the list, e.g. RSS or what was captured.
  readonly detail?: string;
}

export interface ProcessHeaderAttrs {
  readonly title: string;
  readonly options: ReadonlyArray<ProcessOption>;
  readonly selectedKey?: string;
  readonly onSelect: (option: ProcessOption) => void;
  // Optional callback to clear process selection and return to an all-process
  // (system) view. When provided, renders a back button while focused and an
  // all-processes option at the top of the dropdown.
  readonly onClear?: () => void;
  // Whether clearing is currently possible (defaults to selectedKey !== undefined).
  readonly canClear?: boolean;
  // Label for the back button and all-processes menu option.
  readonly clearLabel?: string;
  // Trigger button label when no process is selected.
  readonly unselectedLabel?: string;
  readonly disabled?: boolean;
  // Shown next to the selector, e.g. "waiting for <name>".
  readonly note?: m.Children;
  // Right-aligned controls, e.g. the live session pill.
  readonly trailing?: m.Children;
}

const MAX_LISTED = 200;

// The top bar of every Memscope page: which process the page is about.
export class ProcessHeader implements m.ClassComponent<ProcessHeaderAttrs> {
  private filter = '';

  view({attrs}: m.CVnode<ProcessHeaderAttrs>): m.Children {
    const selected = attrs.options.find((o) => o.key === attrs.selectedKey);
    const clearLabel = attrs.clearLabel ?? 'All processes';
    const showBack =
      attrs.onClear !== undefined && (attrs.canClear ?? selected !== undefined);
    const isSingleStatic =
      selected !== undefined &&
      attrs.options.length === 1 &&
      attrs.onClear === undefined;
    return m(
      '.pf-memscope-process-header',
      m(
        '.pf-memscope-process-header__left',
        m('h1', attrs.title),
        showBack &&
          m(Button, {
            label: clearLabel,
            icon: 'arrow_back',
            variant: ButtonVariant.Outlined,
            disabled: attrs.disabled,
            onclick: () => attrs.onClear?.(),
          }),
        isSingleStatic
          ? m('.pf-memscope-process-header__single', [
              m(Icon, {icon: 'apps'}),
              m('span', selected.name),
            ])
          : m(
              Popup,
              {
                position: PopupPosition.BottomStart,
                trigger: m(Button, {
                  className: 'pf-memscope-process-header__trigger',
                  label:
                    selected?.name ?? attrs.unselectedLabel ?? 'Pick a process',
                  icon: selected !== undefined ? 'apps' : 'devices',
                  rightIcon: 'arrow_drop_down',
                  variant: ButtonVariant.Outlined,
                  disabled: attrs.disabled,
                }),
              },
              this.renderList(attrs),
            ),
        selected !== undefined && m(Chip, {label: `PID ${selected.pid}`}),
        attrs.note !== undefined &&
          m('span.pf-memscope-process-header__note', attrs.note),
      ),
      attrs.trailing,
    );
  }

  private renderList(attrs: ProcessHeaderAttrs): m.Children {
    const needle = this.filter.toLowerCase();
    const clearLabel = attrs.clearLabel ?? 'All processes';
    const showAllItem =
      attrs.onClear !== undefined &&
      (needle === '' ||
        clearLabel.toLowerCase().includes(needle) ||
        'system'.includes(needle));
    const shown = attrs.options
      .filter(
        (o) =>
          needle === '' ||
          o.name.toLowerCase().includes(needle) ||
          `${o.pid}`.includes(needle),
      )
      .slice(0, MAX_LISTED);
    return m(
      '.pf-memscope-process-header__menu',
      m(TextInput, {
        placeholder: 'Search name or PID…',
        leftIcon: 'search',
        value: this.filter,
        autofocus: true,
        onInput: (v: string) => {
          this.filter = v;
        },
      }),
      !showAllItem && shown.length === 0
        ? m('.pf-memscope-placeholder', 'No matching processes.')
        : m(Menu, [
            showAllItem &&
              m(MenuItem, {
                icon: 'devices',
                active: attrs.selectedKey === undefined && !attrs.canClear,
                label: m(
                  'span.pf-memscope-process-header__item',
                  m('span', clearLabel),
                  m(
                    'span.pf-memscope-process-header__secondary',
                    'System overview',
                  ),
                ),
                onclick: () => {
                  this.filter = '';
                  attrs.onClear?.();
                },
              }),
            showAllItem && shown.length > 0 && m(MenuDivider),
            ...shown.map((o) =>
              m(MenuItem, {
                active: o.key === attrs.selectedKey,
                label: m(
                  'span.pf-memscope-process-header__item',
                  m('span', o.name),
                  m(
                    'span.pf-memscope-process-header__secondary',
                    [`PID ${o.pid}`, o.detail].filter((x) => x).join(' · '),
                  ),
                ),
                onclick: () => {
                  this.filter = '';
                  attrs.onSelect(o);
                },
              }),
            ),
          ]),
    );
  }
}
