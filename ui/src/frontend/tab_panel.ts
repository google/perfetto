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
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from './css_constants';
import {DisposableStack} from '../base/disposable_stack';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {assertExists} from '../base/logging';

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

  // NOTE: the visibility state of the tab panel (COLLAPSED, VISIBLE,
  // FULLSCREEN) is stored in TabManagerImpl because it can be toggled via
  // commands. Here we store only the heights for the various states, because
  // nobody else needs to know about them and are an impl. detail of the VDOM.

  // The actual height of the vdom node. It matches resizableHeight if VISIBLE,
  // 0 if COLLAPSED, fullscreenHeight if FULLSCREEN.
  private height = 0;

  // The height when the panel is 'VISIBLE'.
  private resizableHeight = getDefaultDetailsHeight();

  // The height when the panel is 'FULLSCREEN'.
  private fullscreenHeight = 0;

  private fadeContext = new FadeContext();
  private trash = new DisposableStack();

  constructor({attrs}: m.CVnode<TabPanelAttrs>) {
    this.trace = attrs.trace;
  }

  view() {
    const tabMan = this.trace.tabs;
    const tabList = this.trace.tabs.openTabsUri;
    const resolvedTabs = tabMan.resolveTabs(tabList);

    switch (this.trace.tabs.tabPanelVisibility) {
      case 'VISIBLE':
        this.height = Math.min(
          Math.max(this.resizableHeight, 0),
          this.fullscreenHeight,
        );
        break;
      case 'FULLSCREEN':
        this.height = this.fullscreenHeight;
        break;
      case 'COLLAPSED':
        this.height = 0;
        break;
    }

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

    return [
      // Render the header with the ... menu, tab strip and resize buttons.
      m(
        '.handle',
        this.renderTripleDotDropdownMenu(),
        this.renderTabStrip(tabs),
        this.renderTabResizeButtons(),
      ),
      // Render the tab contents.
      m(
        '.details-panel-container',
        {
          style: {height: `${this.height}px`},
        },
        tabs.map(({key, content}) => {
          const active = key === this.trace.tabs.currentTabUri;
          return m(Gate, {open: active}, content);
        }),
      ),
    ];
  }

  oncreate(vnode: m.VnodeDOM<TraceImplAttrs, this>) {
    let dragStartY = 0;
    let heightWhenDragStarted = 0;

    this.trash.use(
      new DragGestureHandler(
        vnode.dom as HTMLElement,
        /* onDrag */ (_x, y) => {
          const deltaYSinceDragStart = dragStartY - y;
          this.resizableHeight = heightWhenDragStarted + deltaYSinceDragStart;
          raf.scheduleFullRedraw();
        },
        /* onDragStarted */ (_x, y) => {
          this.resizableHeight = this.height;
          heightWhenDragStarted = this.height;
          dragStartY = y;
          this.trace.tabs.setTabPanelVisibility('VISIBLE');
        },
        /* onDragFinished */ () => {},
      ),
    );

    const page = assertExists(vnode.dom.parentElement);
    this.fullscreenHeight = page.clientHeight;
    const resizeObs = new ResizeObserver(() => {
      this.fullscreenHeight = page.clientHeight;
      raf.scheduleFullRedraw();
    });
    resizeObs.observe(page);
    this.trash.defer(() => resizeObs.disconnect());
  }

  onremove() {
    this.trash.dispose();
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

  private renderTabResizeButtons(): m.Child {
    const isClosed = this.trace.tabs.tabPanelVisibility === 'COLLAPSED';
    return m(
      '.buttons',
      m(Button, {
        title: 'Open fullscreen',
        disabled: this.trace.tabs.tabPanelVisibility === 'FULLSCREEN',
        icon: 'vertical_align_top',
        compact: true,
        onclick: () => this.trace.tabs.setTabPanelVisibility('FULLSCREEN'),
      }),
      m(Button, {
        onclick: () => this.trace.tabs.toggleTabPanelVisibility(),
        title: isClosed ? 'Show panel' : 'Hide panel',
        icon: isClosed ? 'keyboard_arrow_up' : 'keyboard_arrow_down',
        compact: true,
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

function getDefaultDetailsHeight() {
  const DRAG_HANDLE_HEIGHT_PX = 28;
  // This needs to be a function instead of a const to ensure the CSS constants
  // have been initialized by the time we perform this calculation;
  return DRAG_HANDLE_HEIGHT_PX + DEFAULT_DETAILS_CONTENT_HEIGHT;
}
