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
  // Currently expanded item id (controlled mode).
  // If omitted, the accordion manages its own state (uncontrolled mode).
  readonly expanded?: string;
  // Callback when an item is toggled.
  readonly onToggle?: (id: string | undefined) => void;
  // Space delimited class list applied to the accordion element.
  readonly className?: string;
}

export class Accordion implements m.ClassComponent<AccordionAttrs> {
  // Internal state for uncontrolled mode.
  private internalExpanded: string | undefined = undefined;
  // Track which item should be scrolled into view after render.
  private pendingScrollId: string | undefined = undefined;

  view({attrs}: m.CVnode<AccordionAttrs>): m.Children {
    const {items, onToggle, className} = attrs;

    const expandedId = this.getExpandedId(attrs);

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
                console.log('Scrolled accordion item into view', item.id);
              }
            },
          },
          m(
            '.pf-accordion__header',
            {
              onclick: () => {
                const newExpanded = isExpanded ? undefined : item.id;
                this.internalExpanded = newExpanded;
                // Always scroll the toggled item into view (expand or collapse)
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

  private getExpandedId(attrs: AccordionAttrs): string | undefined {
    // If expanded is provided, use controlled mode.
    // Otherwise use internal state (uncontrolled mode).
    if (attrs.expanded !== undefined) {
      return attrs.expanded;
    }
    return this.internalExpanded;
  }
}
