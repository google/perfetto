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
import {classNames} from '../base/classnames';
import {Icon} from './icon';

export interface AccordionItem {
  // Unique identifier for this item.
  readonly id: string;
  // Content to display in the header (always visible).
  readonly header: m.Children;
  // Content to display when expanded.
  readonly content: m.Children;
}

export interface AccordionAttrs {
  // The list of accordion items to display.
  readonly items: AccordionItem[];
  // Allow multiple items to be open simultaneously. Defaults all items to
  // expanded on first render. Incompatible with controlled mode (expanded).
  readonly multi?: boolean;
  // Currently expanded item id (controlled single-open mode).
  // If omitted, the accordion manages its own state (uncontrolled mode).
  readonly expanded?: string;
  // Callback when an item is toggled.
  readonly onToggle?: (id: string | undefined) => void;
  // Space delimited class list applied to the accordion element.
  readonly className?: string;
}

export class Accordion implements m.ClassComponent<AccordionAttrs> {
  // Internal state for uncontrolled single-open mode.
  private internalExpanded: string | undefined = undefined;
  // Internal state for multi-open mode. Undefined until first render,
  // at which point it is populated with all item ids (all open by default).
  private internalExpandedSet: Set<string> | undefined = undefined;
  // Track which item should be scrolled into view after render.
  private pendingScrollId: string | undefined = undefined;

  view({attrs}: m.CVnode<AccordionAttrs>): m.Children {
    const {items, onToggle, className, multi} = attrs;

    if (multi) {
      if (this.internalExpandedSet === undefined) {
        this.internalExpandedSet = new Set(items.map((i) => i.id));
      }

      return m(
        '.pf-accordion',
        {className},
        items.map((item) => {
          const isExpanded = this.internalExpandedSet!.has(item.id);
          return m(
            '.pf-accordion__item',
            {key: item.id, className: classNames(isExpanded && 'pf-expanded')},
            m(
              '.pf-accordion__header',
              {
                onclick: () => {
                  if (isExpanded) {
                    this.internalExpandedSet!.delete(item.id);
                  } else {
                    this.internalExpandedSet!.add(item.id);
                  }
                  onToggle?.(item.id);
                },
              },
              m(
                '.pf-accordion__toggle',
                m(Icon, {icon: isExpanded ? 'expand_more' : 'chevron_right'}),
              ),
              m('.pf-accordion__header-content', item.header),
            ),
            isExpanded && m('.pf-accordion__content', item.content),
          );
        }),
      );
    }

    const expandedId =
      attrs.expanded !== undefined ? attrs.expanded : this.internalExpanded;

    return m(
      '.pf-accordion',
      {className},
      items.map((item) => {
        const isExpanded = expandedId === item.id;
        const shouldScroll = this.pendingScrollId === item.id;
        return m(
          '.pf-accordion__item',
          {
            key: item.id,
            className: classNames(isExpanded && 'pf-expanded'),
            onupdate: (vnode: m.VnodeDOM) => {
              if (shouldScroll) {
                this.pendingScrollId = undefined;
                const header = vnode.dom as HTMLElement;
                header?.scrollIntoView({behavior: 'instant', block: 'nearest'});
              }
            },
          },
          m(
            '.pf-accordion__header',
            {
              onclick: () => {
                const newExpanded = isExpanded ? undefined : item.id;
                this.internalExpanded = newExpanded;
                this.pendingScrollId = item.id;
                onToggle?.(newExpanded);
              },
            },
            m(
              '.pf-accordion__toggle',
              m(Icon, {icon: isExpanded ? 'expand_more' : 'chevron_right'}),
            ),
            m('.pf-accordion__header-content', item.header),
          ),
          isExpanded && m('.pf-accordion__content', item.content),
        );
      }),
    );
  }
}
