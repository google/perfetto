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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Card} from '../../../widgets/card';
import {TextInput} from '../../../widgets/text_input';
import {Icon} from '../../../widgets/icon';

// Generic widget for a row with name input, validation, and remove button
// Used by all "new column" types
export interface ColumnNameRowAttrs {
  label: string;
  name: string;
  placeholder?: string;
  isValid: boolean;
  onNameChange: (name: string) => void;
  onRemove: () => void;
}

export class ColumnNameRow implements m.ClassComponent<ColumnNameRowAttrs> {
  view({attrs}: m.Vnode<ColumnNameRowAttrs>) {
    const {label, name, placeholder, isValid, onNameChange, onRemove} = attrs;

    return m(
      '.pf-exp-column-name-row',
      m('label', label),
      m(TextInput, {
        oninput: (e: Event) => {
          onNameChange((e.target as HTMLInputElement).value);
        },
        placeholder: placeholder ?? 'name',
        value: name,
      }),
      !isValid && m(Icon, {icon: 'warning'}),
      m(Button, {
        icon: 'close',
        compact: true,
        onclick: onRemove,
      }),
    );
  }
}

// Generic widget for a card with header and action buttons
export interface CardWithHeaderAttrs {
  title: string;
  buttons?: m.Children;
  children: m.Children;
}

export class CardWithHeader implements m.ClassComponent<CardWithHeaderAttrs> {
  view({attrs}: m.Vnode<CardWithHeaderAttrs>) {
    const {title, buttons, children} = attrs;

    return m(
      Card,
      m(
        'div',
        {
          style:
            'display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px',
        },
        m('h2.pf-columns-box-title', {style: 'margin: 0'}, title),
        buttons,
      ),
      children,
    );
  }
}

// Generic button group widget
export interface ButtonGroupAttrs {
  buttons: Array<{
    label: string;
    onclick: () => void;
    variant?: ButtonVariant;
  }>;
}

export class ButtonGroup implements m.ClassComponent<ButtonGroupAttrs> {
  view({attrs}: m.Vnode<ButtonGroupAttrs>) {
    const {buttons} = attrs;

    return m(
      'div.pf-exp-button-group',
      buttons.map((btn) =>
        m(Button, {
          label: btn.label,
          variant: btn.variant ?? ButtonVariant.Outlined,
          onclick: btn.onclick,
        }),
      ),
    );
  }
}

// Generic section with header widget
export interface SectionAttrs {
  title: string;
  headerContent?: m.Children;
  children: m.Children;
}

export class Section implements m.ClassComponent<SectionAttrs> {
  view({attrs}: m.Vnode<SectionAttrs>) {
    const {title, headerContent, children} = attrs;

    return m(
      '.pf-exp-section',
      m('.pf-exp-section-header', m('h2', title), headerContent),
      m('.pf-exp-section-content', children),
    );
  }
}
