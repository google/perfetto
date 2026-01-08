// Copyright (C) 2024 The Android Open Source Project
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
import {TraceImpl} from '../../core/trace_impl';
import {Button} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {DrawerPanel, DrawerTab} from '../../widgets/drawer_panel';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from '../css_constants';
import {CurrentSelectionTab} from './current_selection_tab';

export interface TabPanelAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

export class TabPanel implements m.ClassComponent<TabPanelAttrs> {
  view({
    attrs,
    children,
  }: m.Vnode<TabPanelAttrs, this>): m.Children | null | void {
    const tabMan = attrs.trace.tabs;

    // Build tabs array from registered tabs
    const tabs: DrawerTab[] = [
      // Permanent current selection tab
      {
        key: 'current_selection',
        title: 'Current Selection',
        content: m(CurrentSelectionTab, {trace: attrs.trace}),
      },
      // Dynamic tabs from tab manager
      ...tabMan.resolveTabs(tabMan.openTabsUri).map(({uri, tab}) => ({
        key: uri,
        title: tab?.content.getTitle() ?? 'Tab does not exist',
        content: tab?.content.render(),
        closable: true,
      })),
    ];

    return m(DrawerPanel, {
      className: attrs.className,
      startingHeight: DEFAULT_DETAILS_CONTENT_HEIGHT,
      leftHandleContent: this.renderDropdownMenu(attrs.trace),
      mainContent: children,
      tabs,
      activeTabKey: tabMan.currentTabUri,
      onTabChange: (key) => tabMan.showTab(key),
      onTabClose: (key) => tabMan.hideTab(key),
      visibility: tabMan.tabPanelVisibility,
      onVisibilityChange: (v) => tabMan.setTabPanelVisibility(v),
    });
  }

  private renderDropdownMenu(trace: TraceImpl): m.Child {
    const entries = trace.tabs.tabs
      .filter((tab) => tab.isEphemeral === false)
      .map(({content, uri}) => {
        return {
          key: uri,
          title: content.getTitle(),
          onClick: () => trace.tabs.toggleTab(uri),
          checked: trace.tabs.isOpen(uri),
        };
      });

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: 'more_vert',
          disabled: entries.length === 0,
          title: 'More Tabs',
        }),
      },
      entries.map((entry) => {
        return m(MenuItem, {
          key: entry.key,
          label: entry.title,
          onclick: () => entry.onClick(),
          icon: entry.checked ? 'check_box' : 'check_box_outline_blank',
        });
      }),
    );
  }
}
