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
  title: string;
}

const state = {
  tabs: [
    {id: 'foo', title: 'Foo'},
    {id: 'bar', title: 'Bar'},
    {id: 'baz', title: 'Baz'},
  ] as DemoTab[],
  currentTab: 'foo',
  renamingTab: undefined as string | undefined,
  renameValue: '',
  dragFrom: undefined as number | undefined,
  dragOver: undefined as number | undefined,
};

function closeTab(id: string) {
  state.tabs = state.tabs.filter((tab) => tab.id !== id);
  if (state.currentTab === id) {
    state.currentTab = state.tabs[0]?.id ?? '';
  }
  if (state.renamingTab === id) {
    state.renamingTab = undefined;
  }
}

function commitRename() {
  const tab = state.tabs.find((t) => t.id === state.renamingTab);
  if (tab && state.renameValue.trim() !== '') {
    tab.title = state.renameValue;
  }
  state.renamingTab = undefined;
}

function cancelRename() {
  state.renamingTab = undefined;
}

function startRename(tab: DemoTab) {
  state.renamingTab = tab.id;
  state.renameValue = tab.title;
}

function moveDraggedTab(to: number) {
  const from = state.dragFrom;
  if (from === undefined || from === to) return;
  const [moved] = state.tabs.splice(from, 1);
  state.tabs.splice(to, 0, moved);
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
        const tabs = state.tabs.map((tab, i) =>
          m(
            TabStrip.Tab,
            {
              key: tab.id,
              active: state.currentTab === tab.id,
              onclick: () => (state.currentTab = tab.id),
              ondblclick: opts.renamable ? () => startRename(tab) : undefined,
              leftIcon: opts.icons ? Icons.Search : undefined,
              rightIcon: opts.icons ? Icons.ContextMenuAlt : undefined,
              closeButton: opts.closeButtons,
              onClose: () => closeTab(tab.id),
              menuItems: opts.menus
                ? [
                    m(MenuItem, {
                      label: 'Rename',
                      onclick: () => startRename(tab),
                    }),
                    m(MenuDivider),
                    m(MenuItem, {label: 'Close', onclick: () => closeTab(tab.id)}),
                  ]
                : undefined,
              renaming: state.renamingTab === tab.id,
              renameValue: state.renameValue,
              onRenameInput: (value: string) => (state.renameValue = value),
              onRenameCommit: commitRename,
              onRenameCancel: cancelRename,
              draggable: opts.draggable,
              ondragstart: (e: DragEvent) => {
                state.dragFrom = i;
                e.dataTransfer?.setData('text/plain', tab.id);
              },
              ondragover: (e: DragEvent) => {
                e.preventDefault();
                state.dragOver = i;
              },
              ondragleave: () => {
                if (state.dragOver === i) state.dragOver = undefined;
              },
              ondrop: (e: DragEvent) => {
                e.preventDefault();
                moveDraggedTab(i);
                state.dragFrom = undefined;
                state.dragOver = undefined;
              },
              ondragend: () => {
                state.dragFrom = undefined;
                state.dragOver = undefined;
              },
              className:
                state.dragOver === i
                  ? 'pf-tab-strip__tab--drag-over'
                  : undefined,
            },
            tab.title,
          ),
        );
        return m(TabStrip, {variant: opts.variant}, tabs);
      },
      initialOpts: {
        variant: new EnumOption('card', ['card', 'underline'] as const),
        icons: true,
        closeButtons: true,
        menus: true,
        renamable: true,
        draggable: true,
      },
    }),
  ];
}
