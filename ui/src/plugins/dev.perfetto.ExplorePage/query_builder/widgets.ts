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
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {QueryNode} from '../query_node';
import {NodeModifySection} from './node_explorer_types';

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

// Action button definition for ListItem
export interface ListItemAction {
  label?: string;
  icon: string;
  title?: string;
  onclick: () => void;
}

// Widget for displaying an item with icon, name, description, and action button(s)
// Used in lists of added columns, filters, etc.
export interface ListItemAttrs {
  icon: string;
  name: string;
  description: string;
  actions?: ListItemAction[];
  onRemove?: () => void;
  className?: string;
  onclick?: (event: MouseEvent) => void;
}

export class ListItem implements m.ClassComponent<ListItemAttrs> {
  view({attrs}: m.Vnode<ListItemAttrs>) {
    const {icon, name, description, actions, onRemove, onclick} = attrs;

    return m(
      '.pf-exp-list-item',
      {
        className: attrs.className,
        tabindex: 0,
        role: 'listitem',
        onclick,
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            // Trigger first action on Enter/Space
            actions?.[0]?.onclick();
          } else if (e.key === 'Delete' || e.key === 'Backspace') {
            if (onRemove) {
              e.preventDefault();
              onRemove();
            }
          }
        },
      },
      m(Icon, {icon}),
      m(
        '.pf-exp-list-item-info',
        m('.pf-exp-list-item-name', name),
        m('.pf-exp-list-item-description', description),
      ),
      m('.pf-exp-list-item-actions', this.renderButtons(attrs)),
    );
  }

  private renderButtons(attrs: ListItemAttrs): m.Children {
    const buttons: m.Children = [];

    // Render action buttons
    if (attrs.actions) {
      for (const action of attrs.actions) {
        buttons.push(
          m(Button, {
            label: action.label,
            icon: action.icon,
            title: action.title,
            variant: ButtonVariant.Outlined,
            compact: true,
            onclick: action.onclick,
          }),
        );
      }
    }

    // Remove button
    if (attrs.onRemove) {
      buttons.push(
        m(Button, {
          icon: 'close',
          compact: true,
          onclick: attrs.onRemove,
          title: 'Remove item',
        }),
      );
    }

    return buttons;
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

    // Map icon to Intent for proper styling
    const intentMap: Record<string, Intent> = {
      error: Intent.Danger,
      warning: Intent.Warning,
      info: Intent.Primary,
    };
    const intent = intentMap[icon] ?? Intent.None;

    return m(
      Callout,
      {icon, intent},
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
// Button for switching between basic and advanced modes
// Styled as a subtle, text-like button to indicate it's a secondary action
export interface AdvancedModeChangeButtonAttrs {
  label: string;
  icon?: string;
  onclick: () => void;
}

export class AdvancedModeChangeButton
  implements m.ClassComponent<AdvancedModeChangeButtonAttrs>
{
  view({attrs}: m.Vnode<AdvancedModeChangeButtonAttrs>) {
    const {label, icon, onclick} = attrs;

    return m(Button, {
      className: 'pf-exp-advanced-mode-button',
      label,
      icon,
      onclick,
      variant: ButtonVariant.Minimal,
    });
  }
}

// Widget for equal-width items with responsive stacking
// Items are displayed inline with equal width when there's space,
// and stack vertically when the container is too narrow
export interface EqualWidthRowAttrs {
  separator?: string; // Optional separator text between items
}

export class EqualWidthRow implements m.ClassComponent<EqualWidthRowAttrs> {
  view({attrs, children}: m.CVnode<EqualWidthRowAttrs>) {
    const items = Array.isArray(children) ? children : [children];
    const {separator} = attrs;

    return m(
      '.pf-exp-equal-width-row',
      items.map((item, index) => [
        m('.pf-exp-equal-width-row__item', item),
        separator && index < items.length - 1
          ? m('.pf-exp-equal-width-row__separator', separator)
          : null,
      ]),
    );
  }
}

// Widget for displaying informational text in a styled box
// Similar to the pattern used in timerange node for dynamic mode info
export class InfoBox implements m.ClassComponent {
  view({children}: m.CVnode) {
    return m('.pf-exp-info-box', children);
  }
}

/**
 * Automatically creates error/warning sections from node.state.issues
 * Returns sections to prepend to the node's modify view
 */
export function createErrorSections(node: QueryNode): NodeModifySection[] {
  const sections: NodeModifySection[] = [];

  if (node.state.issues?.queryError) {
    sections.push({
      content: m(
        Callout,
        {icon: 'error'},
        node.state.issues.queryError.message,
      ),
    });
  }

  return sections;
}

/**
 * Reusable list component with empty state and item rendering
 * Used by nodes that display lists of items (filters, aggregations, columns, etc.)
 */
export interface ModifiableItemListAttrs<T> {
  items: T[];
  renderItem: (item: T, index: number) => m.Child;
  emptyStateTitle: string;
  emptyStateIcon?: string;
}

export function ModifiableItemList<T>(
  attrs: ModifiableItemListAttrs<T>,
): m.Child {
  if (attrs.items.length === 0) {
    return m(EmptyState, {
      title: attrs.emptyStateTitle,
      icon: attrs.emptyStateIcon,
    });
  }

  return m(
    '.pf-modifiable-item-list',
    attrs.items.map((item, index) => attrs.renderItem(item, index)),
  );
}

/**
 * Row of action buttons with consistent styling
 * Used for "add item" controls at the top of modify views
 */
export interface ActionButtonRowAttrs {
  buttons: Array<{
    label: string;
    icon: string;
    onclick: () => void;
    variant?: ButtonVariant;
    disabled?: boolean;
  }>;
}

export function ActionButtonRow(attrs: ActionButtonRowAttrs): m.Child {
  return m(
    '.pf-exp-action-buttons',
    attrs.buttons.map((btn) =>
      m(Button, {
        label: btn.label,
        icon: btn.icon,
        onclick: btn.onclick,
        variant: btn.variant ?? ButtonVariant.Outlined,
        disabled: btn.disabled,
      }),
    ),
  );
}

/**
 * Creates a section with a title showing a count
 * Common pattern: "Items (X / Y selected)" or "Items (X)"
 */
export interface CountedSectionTitleAttrs {
  label: string;
  count: number;
  total?: number;
}

export function createCountedSectionTitle(
  attrs: CountedSectionTitleAttrs,
): string {
  if (attrs.total !== undefined) {
    return `${attrs.label} (${attrs.count} / ${attrs.total})`;
  }
  return `${attrs.label} (${attrs.count})`;
}

/**
 * Helper to create a standard "no items" message section
 */
export function createEmptySection(
  title: string,
  icon?: string,
): NodeModifySection {
  return {
    content: m(EmptyState, {
      title,
      icon,
    }),
  };
}

// Widget for inline filter editing form
// Combines editing and creation into a single list item
export interface FormListItemAttrs<T> {
  item: T;
  isValid: boolean;
  onUpdate: (updated: T) => void;
  onRemove?: () => void; // Optional - if not provided, no remove button
  children: m.Children;
}

export class FormListItem<T> implements m.ClassComponent<FormListItemAttrs<T>> {
  view({attrs}: m.Vnode<FormListItemAttrs<T>>) {
    const {isValid, onRemove, children} = attrs;

    return m(
      '.pf-exp-form-list-item',
      {
        className: isValid ? 'pf-valid' : 'pf-invalid',
      },
      m('.pf-exp-form-list-item-content', children),
      // Only show remove button if onRemove is provided
      onRemove &&
        m(
          '.pf-exp-form-list-item-actions',
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: onRemove,
            title: 'Remove item',
          }),
        ),
    );
  }
}

// Material Design outlined input field with label on border
// Pass children as the third argument to m() for select options
export interface OutlinedFieldAttrs {
  label: string;
  value: string;
  onchange?: (e: Event) => void;
  oninput?: (e: Event) => void;
  disabled?: boolean;
  placeholder?: string; // For text inputs
}

export class OutlinedField implements m.ClassComponent<OutlinedFieldAttrs> {
  view({attrs, children}: m.Vnode<OutlinedFieldAttrs>) {
    const {label, value, onchange, oninput, disabled, placeholder} = attrs;

    // Determine if this is a select or input
    // Children can be an array, so check if it has content
    const isSelect =
      children !== undefined &&
      children !== null &&
      (Array.isArray(children) ? children.length > 0 : true);

    return m(
      'fieldset.pf-outlined-field',
      {
        disabled,
      },
      [
        m('legend.pf-outlined-field-legend', label),
        isSelect
          ? m(
              'select.pf-outlined-field-input',
              {
                value,
                onchange,
                disabled,
              },
              children,
            )
          : m('input.pf-outlined-field-input', {
              type: 'text',
              value,
              oninput,
              disabled,
              placeholder,
            }),
      ],
    );
  }
}

// Button styled like an OutlinedField placeholder for adding new items
// Matches the visual style of OutlinedField (border, height, etc.) but acts as a clickable button
export interface AddItemPlaceholderAttrs {
  label: string;
  icon?: string;
  onclick: () => void;
}

export class AddItemPlaceholder
  implements m.ClassComponent<AddItemPlaceholderAttrs>
{
  view({attrs}: m.Vnode<AddItemPlaceholderAttrs>) {
    const {label, icon, onclick} = attrs;

    return m(
      'button.pf-add-item-placeholder',
      {
        type: 'button',
        onclick,
      },
      [
        icon && m(Icon, {icon}),
        m('span.pf-add-item-placeholder__label', label),
      ],
    );
  }
}

// Generic inline editing list widget
// Renders a list of items with inline editing forms, validation, and an "add" button
export interface InlineEditListAttrs<T> {
  items: T[]; // Can include partial/invalid items during editing
  validate: (item: T) => boolean; // Returns true if item is valid
  renderControls: (
    item: T,
    index: number,
    onUpdate: (updated: T) => void,
  ) => m.Children; // Renders the form controls for editing an item
  onUpdate: (items: T[]) => void; // Called when items array changes
  onValidChange?: () => void; // Called when an item becomes valid (for triggering query rebuilds)
  addButtonLabel: string;
  addButtonIcon?: string;
  emptyItem: () => T; // Factory function to create a new empty item
}

export class InlineEditList<T>
  implements m.ClassComponent<InlineEditListAttrs<T>>
{
  view({attrs}: m.Vnode<InlineEditListAttrs<T>>) {
    const {
      items,
      validate,
      renderControls,
      onUpdate,
      onValidChange,
      addButtonLabel,
      addButtonIcon,
      emptyItem,
    } = attrs;

    const itemViews: m.Child[] = [];

    // Render each item as an inline form
    for (const [index, item] of items.entries()) {
      const isValid = validate(item);

      itemViews.push(
        m(FormListItem<T>, {
          item,
          isValid,
          onUpdate: (updated) => {
            const wasValid = validate(item);
            const nowValid = validate(updated);

            // Update the item in the array
            items[index] = updated;
            onUpdate([...items]);

            // If the item just became valid, trigger the valid change callback
            if (!wasValid && nowValid && onValidChange) {
              onValidChange();
            }
          },
          onRemove: () => {
            items.splice(index, 1);
            onUpdate([...items]);
            onValidChange?.();
          },
          children: renderControls(item, index, (updated) => {
            const wasValid = validate(item);
            const nowValid = validate(updated);

            // Update the item
            items[index] = updated;
            onUpdate([...items]);

            // If the item just became valid, trigger the valid change callback
            if (!wasValid && nowValid && onValidChange) {
              onValidChange();
            }
          }),
        }),
      );
    }

    // Add "Add item" button
    itemViews.push(
      m(AddItemPlaceholder, {
        label: addButtonLabel,
        icon: addButtonIcon,
        onclick: () => {
          items.push(emptyItem());
          onUpdate([...items]);
        },
      }),
    );

    return m('.pf-inline-edit-list', itemViews);
  }
}
