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
import {Actions} from '../common/actions';
import {EmptyState} from '../widgets/empty_state';

import {
  DragHandle,
  Tab,
  TabDropdownEntry,
  getDefaultDetailsHeight,
} from './drag_handle';
import {globals} from './globals';
import {raf} from '../core/raf_scheduler';

interface TabWithContent extends Tab {
  content: m.Children;
}

export class TabPanel implements m.ClassComponent {
  // Tabs panel starts collapsed.
  private detailsHeight = 0;
  private fadeContext = new FadeContext();
  private hasBeenDragged = false;

  view() {
    const tabMan = globals.tabManager;
    const tabList = globals.store.state.tabs.openTabs;

    const resolvedTabs = tabMan.resolveTabs(tabList);
    const tabs = resolvedTabs.map(({uri, tab: tabDesc}): TabWithContent => {
      if (tabDesc) {
        const titleStr = tabDesc.content.getTitle();
        return {
          key: uri,
          hasCloseButton: true,
          title: (tabDesc.content.hasContent?.() ?? true) ?
            titleStr :
            m('.pf-nocontent', titleStr),
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

    if (!this.hasBeenDragged &&
        (tabs.length > 0 || globals.state.currentSelection)) {
      this.detailsHeight = getDefaultDetailsHeight();
    }

    // Add the permanent current selection tab to the front of the list of tabs
    tabs.unshift({
      key: 'current_selection',
      title: 'Current Selection',
      content: this.renderCSTabContentWithFading(),
    });

    const tabDropdownEntries =
        globals.tabManager.tabs.filter((tab) => tab.isEphemeral === false)
          .map(({content, uri}): TabDropdownEntry => {
            // Check if the tab is already open
            const isOpen = globals.state.tabs.openTabs.find((openTabUri) => {
              return openTabUri === uri;
            });
            const clickAction = isOpen ?
              Actions.hideTab({uri}) :
              Actions.showTab({uri});
            return {
              key: uri,
              title: content.getTitle(),
              onClick: () => globals.dispatch(clickAction),
              checked: isOpen !== undefined,
            };
          });

    return [
      m(DragHandle, {
        resize: (height: number) => {
          this.detailsHeight = Math.max(height, 0);
          this.hasBeenDragged = true;
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
          const active = (key === globals.state.tabs.currentTab);
          return m(Gate, {open: active}, content);
        }),
      ),
    ];
  }

  private renderCSTabContentWithFading(): m.Children {
    const section = this.renderCSTabContent();
    if (section.isLoading) {
      return m(FadeIn, section.content);
    } else {
      return m(FadeOut, {context: this.fadeContext}, section.content);
    }
  }

  private renderCSTabContent(): {isLoading: boolean, content: m.Children} {
    const cs = globals.state.currentSelection;
    if (!cs) {
      return {
        isLoading: false,
        content: m(EmptyState, {
          className: 'pf-noselection',
          title: 'Nothing selected',
        }, 'Selection details will appear here'),
      };
    }

    const detailsPanels = globals.tabManager.detailsPanels;

    // Get the first "truthy" details panel
    const panel = detailsPanels
      .map((dp) => {
        return {
          content: dp.render(cs),
          isLoading: dp.isLoading?.() ?? false,
        };
      })
      .find(({content}) => content);

    if (panel) {
      return panel;
    } else {
      return {
        isLoading: false,
        content: m(EmptyState, {
          className: 'pf-noselection',
          title: 'No details available',
          icon: 'warning',
        }, `Selection kind: '${cs.kind}'`),
      };
    }
  }
}

const FADE_TIME_MS = 50;

class FadeContext {
  private resolver = () => {};

  putResolver(res: () => void) {
    this.resolver = res;
  }

  resolve() {
    this.resolver();
    this.resolver = () => {};
  }
}

interface FadeOutAttrs {
  context: FadeContext;
}

class FadeOut implements m.ClassComponent<FadeOutAttrs> {
  onbeforeremove({attrs}: m.VnodeDOM<FadeOutAttrs>): Promise<void> {
    return new Promise((res) => {
      attrs.context.putResolver(res);
      setTimeout(res, FADE_TIME_MS);
    });
  }

  oncreate({attrs}: m.VnodeDOM<FadeOutAttrs>) {
    attrs.context.resolve();
  }

  view(vnode: m.Vnode<FadeOutAttrs>): void | m.Children {
    return vnode.children;
  }
}

class FadeIn implements m.ClassComponent {
  private show = false;

  oncreate(_: m.VnodeDOM) {
    setTimeout(() => {
      this.show = true;
      raf.scheduleFullRedraw();
    }, FADE_TIME_MS);
  }

  view(vnode: m.Vnode): m.Children {
    return this.show ? vnode.children : undefined;
  }
}
