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
import {Tab, TabbedSplitPanel} from '../../widgets/tabbed_split_panel';
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
    const tabs = this.gatherTabs(attrs.trace);

    return m(
      TabbedSplitPanel,
      {
        className: attrs.className,
        startingHeight: DEFAULT_DETAILS_CONTENT_HEIGHT,
        leftHandleContent: this.renderDropdownMenu(attrs.trace),
        tabs,
        visibility: attrs.trace.tabs.tabPanelVisibility,
        onVisibilityChange: (visibility) =>
          attrs.trace.tabs.setTabPanelVisibility(visibility),
        onTabChange: (key) => attrs.trace.tabs.showTab(key),
        currentTabKey: attrs.trace.tabs.currentTabUri,
      },
      children,
    );
  }

  private gatherTabs(trace: TraceImpl) {
    const tabMan = trace.tabs;
    const tabList = trace.tabs.openTabsUri;
    const resolvedTabs = tabMan.resolveTabs(tabList);

    const tabs = resolvedTabs.map(({uri, tab: tabDesc}): Tab => {
      if (tabDesc) {
        return {
          key: uri,
          hasCloseButton: true,
          title: tabDesc.content.getTitle(),
          content: tabDesc.content.render(),
          onClose: () => {
            trace.tabs.hideTab(uri);
          },
        };
      } else {
        return {
          key: uri,
          hasCloseButton: true,
          title: 'Tab does not exist',
          content: undefined,
        };
      }
    });

    // Add the permanent current selection tab to the front of the list of tabs
    tabs.unshift({
      key: 'current_selection',
      title: 'Current Selection',
      content: m(CurrentSelectionTab, {trace}),
    });

    return tabs;
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
          compact: true,
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
