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
import {Icon} from './icon';
import {shortUuid} from '../base/uuid';
import {createContext} from '../base/mithril_utils';

const {Consumer, Provider} = createContext<string | undefined>(undefined);

export interface AccordionAttrs {
  // Space delimited class list applied to the accordion element.
  readonly className?: string;
  readonly key?: string;
  readonly multi?: boolean;
}

export class Accordion implements m.ClassComponent<AccordionAttrs> {
  private readonly uuid = shortUuid();
  view({attrs, children}: m.CVnode<AccordionAttrs>): m.Children {
    const {multi, className} = attrs;
    // If multi is false, all sections share the same name and only one can be
    // open at a time. If multi is true, each section gets a unique name and can
    // be opened independently.
    const sharedName = multi ? undefined : this.uuid;
    return m(Provider, {value: sharedName}, [
      m('.pf-accordion', {className}, children),
    ]);
  }
}

export interface AccordionSectionAttrs {
  // Content to display in the summary (always visible).
  readonly summary: m.Children;
  // Space delimited class list applied to the details element.
  readonly className?: string;
  readonly defaultOpen?: boolean;
}

export class AccordionSection
  implements m.ClassComponent<AccordionSectionAttrs>
{
  private isOpen = false;
  private pendingScrollOpen = false;

  constructor({attrs}: m.Vnode<AccordionSectionAttrs>) {
    this.isOpen = attrs.defaultOpen ?? false;
  }

  view({attrs, children}: m.CVnode<AccordionSectionAttrs>): m.Children {
    const {summary: header, className} = attrs;
    return m(Consumer, (groupName) => {
      return m(
        'details.pf-accordion__item',
        {
          className,
          open: this.isOpen,
          ...(groupName != null ? {name: groupName} : {}),
          ontoggle: (e: Event) => {
            this.isOpen = (e.target as HTMLDetailsElement).open;
          },
        },
        m(
          'summary.pf-accordion__header',
          {
            onclick: () => {
              console.log('Clicked header', header);
              // If we're closing this section manually, scroll it into view
              if (this.isOpen) {
                this.pendingScrollOpen = true;
              }
            },
          },
          m('.pf-accordion__toggle', m(Icon, {icon: 'expand_more'})),
          m('.pf-accordion__header-content', header),
        ),
        m('.pf-accordion__content', children),
      );
    });
  }

  onupdate({dom}: m.VnodeDOM<AccordionSectionAttrs>) {
    if (this.pendingScrollOpen) {
      this.pendingScrollOpen = false;
      dom.scrollIntoView({behavior: 'instant', block: 'nearest'});
    }

    // Mithril uses property assignment to clear DOM attributes, which
    // stringifies null to "null". Remove it explicitly.
    if (dom.getAttribute('name') === 'null') {
      dom.removeAttribute('name');
    }
  }
}
