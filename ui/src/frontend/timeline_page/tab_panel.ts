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
import {
  TabPanelVisibility,
  toggleTabPanelVisibility,
} from '../../core/tab_manager';
import {TraceImpl} from '../../core/trace_impl';
import {Button, ButtonBar} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {SplitPanel} from '../../widgets/split_panel';
import {Tab, Tabs} from '../../widgets/tabs';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from '../css_constants';
import {CurrentSelectionTab} from './current_selection_tab';

export interface TabPanelAttrs {
  readonly trace: TraceImpl;
  readonly className?: string;
}

export class TabPanel implements m.ClassComponent<TabPanelAttrs> {
  private drawerHeight = DEFAULT_DETAILS_CONTENT_HEIGHT;
  private containerHeight = 0;

  view({attrs, children}: m.Vnode<TabPanelAttrs, this>): m.Children {
    const trace = attrs.trace;
    const visibility = trace.tabs.tabPanelVisibility;

    // Calculate effective height based on visibility
    let effectiveHeight: number;
    switch (visibility) {
      case TabPanelVisibility.COLLAPSED:
        effectiveHeight = 0;
        break;
      case TabPanelVisibility.FULLSCREEN:
        effectiveHeight = this.containerHeight;
        break;
      case TabPanelVisibility.VISIBLE:
      default:
        effectiveHeight = Math.min(
          Math.max(this.drawerHeight, 0),
          this.containerHeight,
        );
        break;
    }

    const tabs = this.buildTabs(trace);
    const currentTabUri = trace.tabs.currentTabUri;

    return m(SplitPanel, {
      className: attrs.className,
      direction: 'vertical',
      split: {fixed: {panel: 'second', size: effectiveHeight}},
      minSize: 0,
      onResize: (size) => {
        this.drawerHeight = size;
        // When user resizes, switch to VISIBLE mode
        if (visibility !== TabPanelVisibility.VISIBLE) {
          trace.tabs.setTabPanelVisibility(TabPanelVisibility.VISIBLE);
        }
      },
      firstPanel: children,
      secondPanel: m(Tabs, {
        tabs,
        currentTabKey: currentTabUri,
        onTabChange: (key) => trace.tabs.showTab(key),
        leftContent: this.renderDropdownMenu(trace),
        rightContent: this.renderVisibilityButtons(trace, visibility),
        fillHeight: true,
      }),
    });
  }

  oncreate(vnode: m.VnodeDOM<TabPanelAttrs, this>) {
    this.setupResizeObserver(vnode);
  }

  onupdate(vnode: m.VnodeDOM<TabPanelAttrs, this>) {
    // Re-measure container on update in case it changed
    const container = vnode.dom as HTMLElement;
    if (container.parentElement) {
      this.containerHeight = container.parentElement.clientHeight;
    }
  }

  private setupResizeObserver(vnode: m.VnodeDOM<TabPanelAttrs, this>) {
    const container = vnode.dom as HTMLElement;
    if (container.parentElement) {
      this.containerHeight = container.parentElement.clientHeight;
      const resizeObs = new ResizeObserver(() => {
        this.containerHeight = container.parentElement!.clientHeight;
        m.redraw();
      });
      resizeObs.observe(container.parentElement);
    }
  }

  private buildTabs(trace: TraceImpl): Tab[] {
    const tabMan = trace.tabs;
    const tabList = trace.tabs.openTabsUri;
    const resolvedTabs = tabMan.resolveTabs(tabList);
    const currentTabUri = trace.tabs.currentTabUri;

    const tabs: Tab[] = [];

    // Add the permanent current selection tab first
    tabs.push({
      key: 'current_selection',
      title: 'Current Selection',
      content: m(CurrentSelectionTab, {trace}),
    });

    // Add dynamic tabs
    for (const {uri, tab: tabDesc} of resolvedTabs) {
      if (tabDesc) {
        tabs.push({
          key: uri,
          title: tabDesc.content.getTitle(),
          content: currentTabUri === uri ? tabDesc.content.render() : undefined,
          hasCloseButton: true,
          onClose: () => trace.tabs.hideTab(uri),
        });
      } else {
        tabs.push({
          key: uri,
          title: 'Tab does not exist',
          content: undefined,
        });
      }
    }

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

  private renderVisibilityButtons(
    trace: TraceImpl,
    visibility: TabPanelVisibility,
  ): m.Child {
    const isClosed = visibility === TabPanelVisibility.COLLAPSED;
    return m(
      ButtonBar,
      m(Button, {
        title: 'Open fullscreen',
        disabled: visibility === TabPanelVisibility.FULLSCREEN,
        icon: 'vertical_align_top',
        onclick: () => {
          trace.tabs.setTabPanelVisibility(TabPanelVisibility.FULLSCREEN);
        },
      }),
      m(Button, {
        onclick: () => {
          trace.tabs.setTabPanelVisibility(
            toggleTabPanelVisibility(visibility),
          );
        },
        title: isClosed ? 'Show panel' : 'Hide panel',
        icon: isClosed ? 'keyboard_arrow_up' : 'keyboard_arrow_down',
      }),
    );
  }
}
