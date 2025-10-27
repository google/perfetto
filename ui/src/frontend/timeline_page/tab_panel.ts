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
import {Tab, SplitPanel} from '../../widgets/split_panel';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from '../css_constants';
import {CurrentSelectionTab} from './current_selection_tab';
import {Gate} from '../../base/mithril_utils';

export interface TabPanelAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

export class TabPanel implements m.ClassComponent<TabPanelAttrs> {
  view({
    attrs,
    children,
  }: m.Vnode<TabPanelAttrs, this>): m.Children | null | void {
    const {tabs, drawerContent} = this.gatherTabs(attrs.trace);

    return m(
      SplitPanel,
      {
        className: attrs.className,
        startingHeight: DEFAULT_DETAILS_CONTENT_HEIGHT,
        leftHandleContent: this.renderDropdownMenu(attrs.trace),
        tabs,
        drawerContent,
        visibility: attrs.trace.tabs.tabPanelVisibility,
        onVisibilityChange: (visibility) =>
          attrs.trace.tabs.setTabPanelVisibility(visibility),
      },
      children,
    );
  }

  private gatherTabs(trace: TraceImpl) {
    const tabMan = trace.tabs;
    const tabList = trace.tabs.openTabsUri;
    const resolvedTabs = tabMan.resolveTabs(tabList);
    const currentTabUri = trace.tabs.currentTabUri;

    const drawerContent: m.Child[] = [];

    const tabs = resolvedTabs.map(({uri, tab: tabDesc}) => {
      const active = uri === currentTabUri;
      if (tabDesc) {
        drawerContent.push(m(Gate, {open: active}, tabDesc.content.render()));
        return m(
          Tab,
          {
            active,
            onclick: () => trace.tabs.showTab(uri),
            hasCloseButton: true,
            onClose: () => {
              trace.tabs.hideTab(uri);
            },
          },
          tabDesc.content.getTitle(),
        );
      } else {
        return m(
          Tab,
          {
            active,
            onclick: () => trace.tabs.showTab(uri),
          },
          'Tab does not exist',
        );
      }
    });

    // Add the permanent current selection tab to the front of the list of tabs
    const active = currentTabUri === 'current_selection';
    drawerContent.unshift(
      m(Gate, {open: active}, m(CurrentSelectionTab, {trace})),
    );

    tabs.unshift(
      m(
        Tab,
        {
          active,
          onclick: () => trace.tabs.showTab('current_selection'),
        },
        'Current Selection',
      ),
    );

    return {tabs, drawerContent};
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
