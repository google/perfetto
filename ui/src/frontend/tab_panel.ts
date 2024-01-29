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

import {Gate} from '../base/mithril_utils';
import {exists} from '../base/utils';
import {Actions} from '../common/actions';
import {EmptyState} from '../widgets/empty_state';

import {
  DragHandle,
  getDetailsHeight,
  Tab,
  TabDropdownEntry,
} from './drag_handle';
import {globals} from './globals';

interface TabWithContent extends Tab {
  content: m.Children;
}

export class TabPanel implements m.ClassComponent {
  private detailsHeight = getDetailsHeight();

  view() {
    const tabMan = globals.tabManager;
    const tabList = globals.store.state.tabs.openTabs;

    const resolvedTabs = tabMan.resolveTabs(tabList);
    const tabs = resolvedTabs.map(({uri, tab: tabDesc}): TabWithContent => {
      if (tabDesc) {
        return {
          key: uri,
          hasCloseButton: true,
          title: tabDesc.content.getTitle(),
          content: tabDesc.content.render(),
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
      content: this.renderCurrentSelectionTabContent(),
    });

    const tabDropdownEntries =
        globals.tabManager.tabs.filter((tab) => tab.isEphemeral === false)
          .map(({content, uri}): TabDropdownEntry => {
            return {
              key: uri,
              title: content.getTitle(),
              onClick: () => globals.dispatch(Actions.showTab({uri})),
            };
          });

    return [
      m(DragHandle, {
        resize: (height: number) => {
          this.detailsHeight = Math.max(height, 0);
        },
        height: this.detailsHeight,
        tabs,
        currentTabKey: globals.state.tabs.currentTab,
        tabDropdownEntries,
        onTabClick: (key) => globals.dispatch(Actions.showTab({uri: key})),
        onTabClose: (key) => globals.dispatch(Actions.hideTab({uri: key})),
      }),
      m(
        '.details-panel-container',
        {
          style: {height: `${this.detailsHeight}px`},
        },
        tabs.map(({key, content}) => {
          const active = key === globals.state.tabs.currentTab;
          return m(Gate, {open: active}, content);
        }),
      ),
    ];
  }

  private renderCurrentSelectionTabContent(): m.Children {
    const cs = globals.state.currentSelection;
    if (!exists(cs)) {
      return m(EmptyState, {
        className: 'pf-noselection',
        header: 'Nothing selected',
        detail: 'Selection details will appear here',
      });
    }

    const sectionReg = globals.tabManager.detailsPanels;
    const allSections = Array.from(sectionReg.values());

    // Get the first "truthy" current selection section
    const section =
        allSections.map((dp) => dp.render(cs)).find((panel) => panel);

    if (!Boolean(section)) {
      return m(EmptyState, {
        className: 'pf-noselection',
        header: 'No details available',
        detail: `Selection kind: '${cs.kind}'`,
        icon: 'warning',
      });
    } else {
      return section;
    }
  }
}
