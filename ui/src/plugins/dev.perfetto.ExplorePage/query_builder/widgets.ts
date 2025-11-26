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
import {TextParagraph} from '../../../widgets/text_paragraph';
import {SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {perfettoSqlTypeToString} from '../../../trace_processor/perfetto_sql_type';
import {Callout} from '../../../widgets/callout';

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
        '.pf-exp-card-header',
        m('h2.pf-exp-card-header__title', title),
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

// Widget for displaying an item with icon, name, description, and action button
// Used in lists of added columns, filters, etc.
export interface ListItemAttrs {
  icon: string;
  name: string;
  description: string;
  actionLabel: string;
  actionIcon?: string;
  onAction: () => void;
  onRemove?: () => void;
  className?: string;
}

export class ListItem implements m.ClassComponent<ListItemAttrs> {
  view({attrs}: m.Vnode<ListItemAttrs>) {
    const {
      icon,
      name,
      description,
      actionLabel,
      actionIcon,
      onAction,
      onRemove,
    } = attrs;

    return m(
      '.pf-exp-list-item',
      {className: attrs.className},
      m(Icon, {icon}),
      m(
        '.pf-exp-list-item-info',
        m('.pf-exp-list-item-name', name),
        m('.pf-exp-list-item-description', description),
      ),
      m(
        '.pf-exp-list-item-actions',
        m(Button, {
          label: actionLabel,
          icon: actionIcon,
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: onAction,
        }),
        onRemove &&
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: onRemove,
          }),
      ),
    );
  }
}

// Widget for a horizontal row of action buttons
export interface ActionButtonsAttrs {
  buttons: Array<{
    label: string;
    icon?: string;
    onclick: () => void;
    active?: boolean;
  }>;
}

export class ActionButtons implements m.ClassComponent<ActionButtonsAttrs> {
  view({attrs}: m.Vnode<ActionButtonsAttrs>) {
    return m(
      '.pf-exp-action-buttons',
      attrs.buttons.map((btn) =>
        m(Button, {
          label: btn.active ? `${btn.label} ✓` : btn.label,
          icon: btn.icon,
          variant: ButtonVariant.Outlined,
          onclick: btn.onclick,
        }),
      ),
    );
  }
}

// Widget for a labeled form row with input
// The children are placed inside the label for proper accessibility
export interface FormRowAttrs {
  label: string;
}

export class FormRow implements m.ClassComponent<FormRowAttrs> {
  view({attrs, children}: m.CVnode<FormRowAttrs>) {
    return m('label.pf-exp-form-row', m('span', attrs.label), children);
  }
}

// Widget for inline label with any control (input, select, multiselect, etc.)
// The children are placed inside the label for proper accessibility
export interface LabeledControlAttrs {
  label: string;
}

export class LabeledControl implements m.ClassComponent<LabeledControlAttrs> {
  view({attrs, children}: m.CVnode<LabeledControlAttrs>) {
    return m('label.pf-exp-labeled-control', m('span', attrs.label), children);
  }
}

// Widget for a draggable list item with drag handle
export interface DraggableItemAttrs {
  index: number;
  onReorder: (fromIndex: number, toIndex: number) => void;
}

export class DraggableItem implements m.ClassComponent<DraggableItemAttrs> {
  view({attrs, children}: m.CVnode<DraggableItemAttrs>) {
    const {index, onReorder} = attrs;

    return m(
      '.pf-exp-draggable-item',
      {
        draggable: true,
        ondragstart: (e: DragEvent) => {
          e.dataTransfer?.setData('text/plain', index.toString());
        },
        ondragover: (e: DragEvent) => {
          e.preventDefault();
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          const from = parseInt(
            e.dataTransfer?.getData('text/plain') ?? '0',
            10,
          );
          onReorder(from, index);
        },
      },
      m('span.pf-exp-drag-handle', '☰'),
      children,
    );
  }
}

// Widget for displaying a list of issues (errors or warnings) in a callout
// Used for validation errors, duplicate column warnings, etc.
export interface IssueListAttrs {
  icon: 'error' | 'warning' | 'info';
  title: string;
  items: string[];
}

export class IssueList implements m.ClassComponent<IssueListAttrs> {
  view({attrs}: m.Vnode<IssueListAttrs>) {
    const {icon, title, items} = attrs;

    if (items.length === 0) {
      return null;
    }

    return m(
      Callout,
      {icon},
      m('div', title),
      m(
        'ul.pf-exp-issue-list',
        items.map((item) => m('li', item)),
      ),
    );
  }
}

// Widget for displaying a SQL table's description and columns
// Used in table source node info and join modal
export interface TableDescriptionAttrs {
  table: SqlTable;
}

export class TableDescription
  implements m.ClassComponent<TableDescriptionAttrs>
{
  view({attrs}: m.Vnode<TableDescriptionAttrs>) {
    const {table} = attrs;

    return m(
      '.pf-exp-table-description',
      m(TextParagraph, {text: table.description}),
      m(
        'table.pf-table.pf-table-striped',
        m(
          'thead',
          m('tr', m('th', 'Column'), m('th', 'Type'), m('th', 'Description')),
        ),
        m(
          'tbody',
          table.columns.map((col) => {
            return m(
              'tr',
              m('td', col.name),
              m('td', perfettoSqlTypeToString(col.type)),
              m('td', col.description),
            );
          }),
        ),
      ),
    );
  }
}
