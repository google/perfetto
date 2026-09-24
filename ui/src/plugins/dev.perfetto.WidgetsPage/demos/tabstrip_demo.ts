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
import {TabStrip} from '../../../widgets/tab_strip';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {Icons} from '../../../base/semantic_icons';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

interface DemoTab {
  readonly id: string;
  readonly title: string;
}

const TABS: ReadonlyArray<DemoTab> = [
  {id: 'foo', title: 'Foo'},
  {id: 'bar', title: 'Bar'},
  {id: 'baz', title: 'Baz'},
];
const ACTIVE_TAB = 'foo';

const callbackLog: string[] = [];

function logCallback(msg: string) {
  const time = new Date().toLocaleTimeString();
  callbackLog.push(`[${time}] ${msg}`);
}

export function renderTabStrip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TabStrip'),
      m(
        'p',
        'A horizontal tab navigation component for switching between different views or sections.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const tabs = TABS.map((tab) =>
          m(
            TabStrip.Tab,
            {
              key: tab.id,
              active: ACTIVE_TAB === tab.id,
              disabled: opts.disabledTab && tab.id === 'bar',
              href: opts.links ? `https://example.com/${tab.id}` : undefined,
              onclick: (e: PointerEvent) => {
                // Don't actually follow the bogus link.
                e.preventDefault();
                logCallback(`onclick: ${tab.id}`);
              },
              leftIcon: opts.leftIcon ? Icons.Search : undefined,
              rightIcon: opts.rightIcon ? Icons.Info : undefined,
              closeButton: opts.closeButtons,
              onClose: () => logCallback(`onClose: ${tab.id}`),
              menuItems: opts.menus
                ? [
                    m(MenuItem, {label: 'Menu item 1'}),
                    m(MenuItem, {label: 'Menu item 2'}),
                    m(MenuDivider),
                    m(MenuItem, {label: 'Menu item 3'}),
                  ]
                : undefined,
              onRename: opts.renamable
                ? (newName: string) =>
                    logCallback(`onRename: ${tab.id} -> ${newName}`)
                : undefined,
            },
            tab.title,
          ),
        );
        return m(
          TabStrip,
          {
            variant: opts.variant,
            reorderable: opts.reorderable,
            onReorder: (from: number, to: number) =>
              logCallback(`onReorder: ${from} -> ${to}`),
          },
          tabs,
        );
      },
      initialOpts: {
        variant: new EnumOption('card', ['card', 'underline'] as const),
        leftIcon: false,
        rightIcon: false,
        closeButtons: false,
        menus: false,
        renamable: false,
        disabledTab: false,
        reorderable: false,
        links: false,
      },
    }),
    m(
      'pre',
      {
        style: {
          height: '150px',
          overflowY: 'auto',
          margin: '0',
        },
        onupdate: (vnode: m.VnodeDOM) => {
          const el = vnode.dom as HTMLElement;
          el.scrollTop = el.scrollHeight;
        },
      },
      callbackLog.length === 0
        ? 'Callbacks will appear here'
        : callbackLog.join('\n'),
    ),
  ];
}
