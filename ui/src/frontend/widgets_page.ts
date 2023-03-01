// Copyright (C) 2023 The Android Open Source Project
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

import * as m from 'mithril';
import {Anchor} from './anchor';
import {globals} from './globals';
import {createPage} from './pages';
import {Button} from './widgets/button';
import {Checkbox} from './widgets/checkbox';
import {EmptyState} from './widgets/empty_state';
import {TextInput} from './widgets/text_input';

interface WidgetShowcaseAttrs {
  initialOpts: any;
  renderWidget: (options: any) => any;
}

// A little helper class to render any vnode with a dynamic set of options
class WidgetShowcase implements m.ClassComponent<WidgetShowcaseAttrs> {
  private opts?: any;
  view({attrs}: m.CVnode<WidgetShowcaseAttrs>) {
    const {
      initialOpts,
      renderWidget,
    } = attrs;

    const opts = this.opts || initialOpts;

    const listItems = [];
    // eslint-disable-next-line guard-for-in
    for (const key in opts) {
      const val = opts[key];
      if (typeof val === 'boolean') {
        listItems.push(
            m('li', m(Checkbox, {
                checked: opts[key],
                label: key,
                onchange: () => {
                  opts[key] = !opts[key];
                  globals.rafScheduler.scheduleFullRedraw();
                },
              })),
        );
      }
    }

    return [
      m(
          '.widget-block',
          m(
              '.widget-container',
              renderWidget(opts),
              ),
          m(
              '.widget-controls',
              m('h3', 'Options'),
              m('ul', listItems),
              ),
          ),
    ];
  }

  oninit({attrs}: m.Vnode<WidgetShowcaseAttrs, this>) {
    this.opts = attrs.initialOpts;
  }
}

export const WidgetsPage = createPage({
  view() {
    return m(
        '.widgets-page',
        m('h1', 'Widgets'),
        m('h2', 'Button'),
        m(WidgetShowcase, {
          renderWidget: ({label, icon, ...rest}) => m(Button, {
            icon: icon ? 'send' : undefined,
            label: label ? 'Button' : '',
            ...rest,
          }),
          initialOpts: {
            label: true,
            icon: false,
            disabled: false,
            minimal: false,
            active: false,
            compact: false,
          },
        }),
        m('h2', 'Checkbox'),
        m(WidgetShowcase, {
          renderWidget: (opts) => m(Checkbox, {label: 'Checkbox', ...opts}),
          initialOpts: {
            disabled: false,
          },
        }),
        m('h2', 'Text Input'),
        m(WidgetShowcase, {
          renderWidget: ({placeholder, ...rest}) => m(TextInput, {
            placeholder: placeholder ? 'Placeholder...' : '',
            ...rest,
          }),
          initialOpts: {
            placeholder: true,
            disabled: false,
          },
        }),
        m('h2', 'Empty State'),
        m(WidgetShowcase, {
          renderWidget: ({header, content}) =>
              m(EmptyState,
                {
                  header: header && 'No search results found...',
                },
                content && m(Button, {label: 'Try again'})),
          initialOpts: {
            header: true,
            content: true,
          },
        }),
        m('h2', 'Anchor'),
        m(WidgetShowcase, {
          renderWidget: ({icon}) => m(
              Anchor,
              {
                icon: icon && 'open_in_new',
                href: 'https://perfetto.dev/docs/',
                target: '_blank',
              },
              'Docs',
              ),
          initialOpts: {
            icon: true,
          },
        }),
    );
  },
});
