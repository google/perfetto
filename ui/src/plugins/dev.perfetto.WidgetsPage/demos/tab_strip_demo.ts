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
import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {TabStrip, TabStripLink, TabStripTab} from '../../../widgets/tab_strip';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

const TABS = [
  {key: 'foo', title: 'Foo', icon: Icons.Info},
  {key: 'bar', title: 'Bar', icon: Icons.Chart},
  {key: 'baz', title: 'Baz', icon: Icons.Search},
];

let currentTab = 'foo';

export function renderTabStrip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TabBar'),
      m(
        'p',
        'A composable tab bar. It renders only the bar and its styling; put ' +
          'TabBarTab, TabBarLink, buttons, or any other content inside it. ' +
          'Each item says whether it is active, so the bar can be driven by ' +
          'a router or any other state. For a component that also manages ' +
          'tab content and active state, use Tabs.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(
          '',
          {style: {width: '500px'}},
          m(
            TabStrip,
            {
              variant: opts.variant,
              leftContent: opts.leftContent
                ? m(Button, {icon: Icons.Menu})
                : undefined,
              rightContent: opts.rightContent
                ? m(Button, {icon: Icons.Filter, label: 'Filter'})
                : undefined,
            },
            TABS.map(({key, title, icon}) => {
              const common = {
                key,
                active: currentTab === key,
                icon: opts.showIcons ? icon : undefined,
              };
              // Links point back at this demo page so it stays put; onclick
              // stands in for the URL-derived active state a router would give.
              return opts.links
                ? m(
                    TabStripLink,
                    {
                      ...common,
                      href: `#!/widgets/tabbar`,
                      onclick: () => {
                        currentTab = key;
                      },
                    },
                    title,
                  )
                : m(
                    TabStripTab,
                    {
                      ...common,
                      onclick: () => {
                        currentTab = key;
                      },
                      onClose: opts.closeButton
                        ? () => console.log(`Close ${key}`)
                        : undefined,
                    },
                    title,
                  );
            }),
            opts.newTabButton &&
              m(Button, {
                icon: Icons.Add,
                onclick: () => console.log('New tab'),
              }),
          ),
        );
      },
      initialOpts: {
        variant: new EnumOption('card', ['card', 'underline'] as const),
        links: false,
        showIcons: true,
        closeButton: false,
        newTabButton: true,
        leftContent: false,
        rightContent: false,
      },
    }),
  ];
}
