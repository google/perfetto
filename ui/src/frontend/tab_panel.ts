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
import {EmptyState} from '../widgets/empty_state';
import {raf} from '../core/raf_scheduler';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {Tree, TreeNode} from '../widgets/tree';
import {TraceImpl, TraceImplAttrs} from '../core/trace_impl';
import {MenuItem, PopupMenu2} from '../widgets/menu';
import {Button} from '../widgets/button';
import {CollapsiblePanel} from './widgets/collapsible_panel';

export type TabPanelAttrs = TraceImplAttrs;

export interface Tab {
  // Unique key for this tab, passed to callbacks.
  key: string;

  // Tab title to show on the tab handle.
  title: m.Children;

  // Whether to show a close button on the tab handle or not.
  // Default = false.
  hasCloseButton?: boolean;
}

interface TabWithContent extends Tab {
  content: m.Children;
}

export interface TabDropdownEntry {
  // Unique key for this tab dropdown entry.
  key: string;

  // Title to show on this entry.
  title: string;

  // Called when tab dropdown entry is clicked.
  onClick: () => void;

  // Whether this tab is checked or not
  checked: boolean;
}

export class TabPanel implements m.ClassComponent<TabPanelAttrs> {
  private readonly trace: TraceImpl;
  // Tabs panel starts collapsed.

  private fadeContext = new FadeContext();

  constructor({attrs}: m.CVnode<TabPanelAttrs>) {
    this.trace = attrs.trace;
  }

  view() {
    const tabMan = this.trace.tabs;
    const tabList = this.trace.tabs.openTabsUri;
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
      content: this.renderCSTabContentWithFading(),
    });

    return m(CollapsiblePanel, {
      visibility: this.trace.tabs.tabPanelVisibility,
      setVisibility: (visibility) =>
        this.trace.tabs.setTabPanelVisibility(visibility),
      headerActions: [
        this.renderTripleDotDropdownMenu(),
        this.renderTabStrip(tabs),
      ],
      tabs: tabs.map(({key, content}) => {
        const active = key === this.trace.tabs.currentTabUri;
        return m(Gate, {open: active}, content);
      }),
    });
  }

  private renderTripleDotDropdownMenu(): m.Child {
    const entries = this.trace.tabs.tabs
      .filter((tab) => tab.isEphemeral === false)
      .map(({content, uri}): TabDropdownEntry => {
        return {
          key: uri,
          title: content.getTitle(),
          onClick: () => this.trace.tabs.toggleTab(uri),
          checked: this.trace.tabs.isOpen(uri),
        };
      });

    return m(
      '.buttons',
      m(
        PopupMenu2,
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
      ),
    );
  }

  private renderTabStrip(tabs: Tab[]): m.Child {
    const currentTabKey = this.trace.tabs.currentTabUri;
    return m(
      '.tabs',
      tabs.map((tab) => {
        const {key, hasCloseButton = false} = tab;
        const tag = currentTabKey === key ? '.tab[active]' : '.tab';
        return m(
          tag,
          {
            key,
            onclick: (event: Event) => {
              if (!event.defaultPrevented) {
                this.trace.tabs.showTab(key);
              }
            },
            // Middle click to close
            onauxclick: (event: MouseEvent) => {
              if (!event.defaultPrevented) {
                this.trace.tabs.hideTab(key);
              }
            },
          },
          m('span.pf-tab-title', tab.title),
          hasCloseButton &&
            m(Button, {
              onclick: (event: Event) => {
                this.trace.tabs.hideTab(key);
                event.preventDefault();
              },
              compact: true,
              icon: 'close',
            }),
        );
      }),
    );
  }

  private renderCSTabContentWithFading(): m.Children {
    const section = this.renderCSTabContent();
    if (section.isLoading) {
      return m(FadeIn, section.content);
    } else {
      return m(FadeOut, {context: this.fadeContext}, section.content);
    }
  }

  private renderCSTabContent(): {isLoading: boolean; content: m.Children} {
    const currentSelection = this.trace.selection.selection;
    if (currentSelection.kind === 'empty') {
      return {
        isLoading: false,
        content: m(
          EmptyState,
          {
            className: 'pf-noselection',
            title: 'Nothing selected',
          },
          'Selection details will appear here',
        ),
      };
    }

    if (currentSelection.kind === 'track') {
      return {
        isLoading: false,
        content: this.renderTrackDetailsPanel(currentSelection.trackUri),
      };
    }

    const detailsPanel = this.trace.selection.getDetailsPanelForSelection();
    if (currentSelection.kind === 'track_event' && detailsPanel !== undefined) {
      return {
        isLoading: detailsPanel.isLoading,
        content: detailsPanel.render(),
      };
    }

    // Get the first "truthy" details panel
    const detailsPanels = this.trace.tabs.detailsPanels.map((dp) => {
      return {
        content: dp.render(currentSelection),
        isLoading: dp.isLoading?.() ?? false,
      };
    });

    const panel = detailsPanels.find(({content}) => content);

    if (panel) {
      return panel;
    } else {
      return {
        isLoading: false,
        content: m(
          EmptyState,
          {
            className: 'pf-noselection',
            title: 'No details available',
            icon: 'warning',
          },
          `Selection kind: '${currentSelection.kind}'`,
        ),
      };
    }
  }

  private renderTrackDetailsPanel(trackUri: string) {
    const track = this.trace.tracks.getTrack(trackUri);
    if (track) {
      return m(
        DetailsShell,
        {title: 'Track', description: track.title},
        m(
          GridLayout,
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Details'},
              m(
                Tree,
                m(TreeNode, {left: 'Name', right: track.title}),
                m(TreeNode, {left: 'URI', right: track.uri}),
                m(TreeNode, {left: 'Plugin ID', right: track.pluginId}),
                m(
                  TreeNode,
                  {left: 'Tags'},
                  track.tags &&
                    Object.entries(track.tags).map(([key, value]) => {
                      return m(TreeNode, {left: key, right: value?.toString()});
                    }),
                ),
              ),
            ),
          ),
        ),
      );
    } else {
      return undefined; // TODO show something sensible here
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
