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
import {assertUnreachable} from '../base/assert';
import {Gate, MithrilEvent} from '../base/mithril_utils';
import {Button, ButtonBar} from './button';
import {classNames} from '../base/classnames';
import {HTMLAttrs} from './common';
import {Icons} from '../base/semantic_icons';

export interface TabAttrs extends HTMLAttrs {
  // Is this tab currently active?
  readonly active?: boolean;
  // Whether to show a close button on the tab.
  readonly hasCloseButton?: boolean;
  // What happens when the close button is clicked.
  readonly onClose?: () => void;
}

export class Tab implements m.ClassComponent<TabAttrs> {
  view({attrs, children}: m.CVnode<TabAttrs>): m.Children {
    const {active, hasCloseButton, ...rest} = attrs;
    return m(
      '.pf-drawer-panel__tab',
      {
        ...rest,
        className: classNames(active && 'pf-drawer-panel__tab--active'),
        onauxclick: () => {
          attrs.onClose?.();
        },
      },
      m('.pf-drawer-panel__tab-title', children),
      hasCloseButton &&
        m(Button, {
          compact: true,
          icon: Icons.Close,
          onclick: (e) => {
            e.stopPropagation();
            attrs.onClose?.();
          },
        }),
    );
  }
}

export interface DrawerTab {
  // Unique identifier for the tab.
  readonly key: string;
  // Content to display in the tab handle.
  readonly title: m.Children;
  // Content to display in the drawer when this tab is active.
  readonly content: m.Children;
  // Whether to show a close button on the tab.
  readonly closable?: boolean;
}

export enum DrawerPanelVisibility {
  VISIBLE,
  FULLSCREEN,
  COLLAPSED,
}

export interface DrawerPanelAttrs {
  // Content to display in the main area (above the drawer).
  readonly mainContent?: m.Children;

  // Content to put to the left of the tabs on the handle.
  readonly leftHandleContent?: m.Children;

  // ===== Simple mode (no tab bar) =====
  // Content to display inside the drawer.
  readonly drawerContent?: m.Children;

  // ===== Tabs mode (with tab bar) =====
  // If provided, ignores drawerContent and renders tabs instead.
  readonly tabs?: DrawerTab[];

  // The currently active tab key (controlled mode).
  // If not provided, the component manages its own state (uncontrolled mode).
  readonly activeTabKey?: string;

  // Called when a tab is clicked.
  onTabChange?(key: string): void;

  // Called when a tab's close button is clicked.
  onTabClose?(key: string): void;

  // ===== Common options =====
  // Whether the drawer is currently visible or not (when in controlled mode).
  readonly visibility?: DrawerPanelVisibility;

  // Extra classes applied to the root element.
  readonly className?: string;

  // What height should the drawer be initially?
  readonly startingHeight?: number;

  // Called when the drawer visibility is changed.
  onVisibilityChange?(visibility: DrawerPanelVisibility): void;
}

/**
 * A container that fills its parent container, with a main content area and a
 * collapsible drawer at the bottom. The main content is specified via the
 * `mainContent` attribute, and the drawer content via `drawerContent`.
 *
 * The drawer features a handle that can be dragged to adjust the height of the
 * drawer, and also features buttons to maximize and minimise the drawer.
 *
 * Content can also optionally be displayed on the handle itself to the left of
 * the buttons.
 *
 * The layout looks like this:
 *
 * ┌───────────────────────────────────────────────────────────────────┐
 * │pf-drawer-panel                                                    │
 * │┌─────────────────────────────────────────────────────────────────┐|
 * ││pf-drawer-panel__main                                            ││
 * |└─────────────────────────────────────────────────────────────────┘|
 * │┌─────────────────────────────────────────────────────────────────┐|
 * ││pf-drawer-panel__handle                                          ││
 * │|┌─────────────────┐┌──────────────────────┐┌────────────────────┐||
 * |||leftHandleContent||.pf-drawer-panel__tabs||.pf-button-bar      |||
 * ||└─────────────────┘└──────────────────────┘└────────────────────┘||
 * |└─────────────────────────────────────────────────────────────────┘|
 * │┌─────────────────────────────────────────────────────────────────┐|
 * ││pf-drawer-panel__drawer                                          ││
 * |└─────────────────────────────────────────────────────────────────┘|
 * └───────────────────────────────────────────────────────────────────┘
 */
export class DrawerPanel implements m.ClassComponent<DrawerPanelAttrs> {
  // The actual height of the vdom node. It matches resizableHeight if VISIBLE,
  // 0 if COLLAPSED, fullscreenHeight if FULLSCREEN.
  private height = 0;

  // The height when the panel is 'VISIBLE'.
  private resizableHeight: number;

  // The height when the panel is 'FULLSCREEN'.
  private fullscreenHeight = 0;

  // Current visibility state (if not controlled).
  private visibility = DrawerPanelVisibility.VISIBLE;

  // Current active tab key (for uncontrolled mode).
  private internalActiveTab?: string;

  // For pointer capture drag handling
  private handleElement?: HTMLElement;
  private dragStartY?: number;
  private heightWhenDragStarted = 0;
  private pendingPointerId?: number;
  private resizeObserver?: ResizeObserver;

  constructor({attrs}: m.CVnode<DrawerPanelAttrs>) {
    this.resizableHeight = attrs.startingHeight ?? 100;
  }

  view({attrs}: m.CVnode<DrawerPanelAttrs>) {
    const {
      mainContent,
      leftHandleContent,
      drawerContent,
      tabs,
      activeTabKey,
      onTabChange,
      onTabClose,
      visibility = this.visibility,
      className,
      onVisibilityChange,
    } = attrs;

    switch (visibility) {
      case DrawerPanelVisibility.VISIBLE:
        this.height = Math.min(
          Math.max(this.resizableHeight, 0),
          this.fullscreenHeight,
        );
        break;
      case DrawerPanelVisibility.FULLSCREEN:
        this.height = this.fullscreenHeight;
        break;
      case DrawerPanelVisibility.COLLAPSED:
        this.height = 0;
        break;
    }

    // Determine mode: tabs mode if tabs array is provided and non-empty
    const isTabsMode = tabs !== undefined && tabs.length > 0;

    // Get active tab key (controlled or uncontrolled)
    const activeKey = isTabsMode
      ? activeTabKey ?? this.internalActiveTab ?? tabs[0].key
      : undefined;

    // Render tabs UI and drawer content based on mode
    const tabsUI = isTabsMode
      ? this.renderTabs(tabs, activeKey!, onTabChange, onTabClose)
      : undefined;
    const drawer = isTabsMode
      ? this.renderTabContent(tabs, activeKey!)
      : drawerContent;

    return m(
      '.pf-drawer-panel',
      {
        className,
      },
      m('.pf-drawer-panel__main', mainContent),
      m(
        '.pf-drawer-panel__handle',
        {
          oncontextmenu: (e: Event) => e.preventDefault(),
          onpointerdown: (e: PointerEvent) => this.onPointerDown(e, attrs),
          onpointermove: (e: MithrilEvent<PointerEvent>) =>
            this.onPointerMove(e),
          onpointerup: (e: PointerEvent) => this.onPointerUp(e),
          onpointercancel: (e: PointerEvent) => this.onPointerCancel(e),
          onpointercapturelost: (e: PointerEvent) =>
            this.onPointerCaptureLost(e),
        },
        [
          leftHandleContent,
          m('.pf-drawer-panel__tabs', tabsUI),
          this.renderTabResizeButtons(visibility, onVisibilityChange),
        ],
      ),
      m(
        '.pf-drawer-panel__drawer',
        {
          style: {height: `${this.height}px`},
        },
        drawer,
      ),
    );
  }

  private renderTabs(
    tabs: DrawerTab[],
    activeKey: string,
    onTabChange?: (key: string) => void,
    onTabClose?: (key: string) => void,
  ): m.Children {
    return tabs.map((tab) =>
      m(
        Tab,
        {
          active: tab.key === activeKey,
          hasCloseButton: tab.closable,
          onclick: () => {
            this.internalActiveTab = tab.key;
            onTabChange?.(tab.key);
          },
          onClose: () => onTabClose?.(tab.key),
        },
        tab.title,
      ),
    );
  }

  private renderTabContent(tabs: DrawerTab[], activeKey: string): m.Children {
    return tabs.map((tab) =>
      m(Gate, {open: tab.key === activeKey}, tab.content),
    );
  }

  oncreate(vnode: m.VnodeDOM<DrawerPanelAttrs, this>) {
    const parent = vnode.dom.parentElement;
    if (parent) {
      this.fullscreenHeight = parent.clientHeight;
      this.resizeObserver = new ResizeObserver(() => {
        this.fullscreenHeight = parent.clientHeight;
        m.redraw();
      });
      this.resizeObserver.observe(parent);
    }
  }

  onremove() {
    this.resizeObserver?.disconnect();
  }

  private endDrag(pointerId: number) {
    if (this.dragStartY !== undefined) {
      this.dragStartY = undefined;
      this.pendingPointerId = undefined;
      if (this.handleElement?.hasPointerCapture(pointerId)) {
        this.handleElement.releasePointerCapture(pointerId);
      }
    }
  }

  private onPointerDown(e: PointerEvent, attrs: DrawerPanelAttrs) {
    this.handleElement = e.currentTarget as HTMLElement;
    this.dragStartY = e.clientY;
    this.resizableHeight = this.height;
    this.heightWhenDragStarted = this.height;
    // Defer setPointerCapture to the first pointermove. Capturing on
    // pointerdown redirects pointerup (and the derived click event) to the
    // handle element, which prevents onclick handlers on child elements (e.g.
    // tabs) from firing.
    this.pendingPointerId = e.pointerId;
    this.updatePanelVisibility(
      DrawerPanelVisibility.VISIBLE,
      attrs.onVisibilityChange,
    );
    e.stopPropagation();
  }

  private onPointerMove(e: MithrilEvent<PointerEvent>) {
    e.redraw = false;
    if (this.dragStartY !== undefined) {
      if (this.pendingPointerId !== undefined && this.handleElement) {
        this.handleElement.setPointerCapture(this.pendingPointerId);
        this.pendingPointerId = undefined;
      }
      const deltaY = this.dragStartY - e.clientY;
      this.resizableHeight = this.heightWhenDragStarted + deltaY;
      m.redraw();
    }
  }

  private onPointerUp(e: PointerEvent) {
    this.endDrag(e.pointerId);
  }

  private onPointerCancel(e: PointerEvent) {
    this.endDrag(e.pointerId);
  }

  private onPointerCaptureLost(e: PointerEvent) {
    this.endDrag(e.pointerId);
  }

  private renderTabResizeButtons(
    visibility: DrawerPanelVisibility,
    setVisibility?: (visibility: DrawerPanelVisibility) => void,
  ): m.Child {
    const isClosed = visibility === DrawerPanelVisibility.COLLAPSED;
    return m(
      ButtonBar,
      m(Button, {
        title: 'Open fullscreen',
        disabled: visibility === DrawerPanelVisibility.FULLSCREEN,
        icon: 'vertical_align_top',
        onclick: () => {
          this.updatePanelVisibility(
            DrawerPanelVisibility.FULLSCREEN,
            setVisibility,
          );
        },
      }),
      m(Button, {
        onclick: () => {
          this.updatePanelVisibility(
            toggleVisibility(visibility),
            setVisibility,
          );
        },
        title: isClosed ? 'Show panel' : 'Hide panel',
        icon: isClosed ? 'keyboard_arrow_up' : 'keyboard_arrow_down',
      }),
    );
  }

  private updatePanelVisibility(
    visibility: DrawerPanelVisibility,
    setVisibility?: (visibility: DrawerPanelVisibility) => void,
  ) {
    this.visibility = visibility;
    setVisibility?.(visibility);
  }
}

export function toggleVisibility(visibility: DrawerPanelVisibility) {
  switch (visibility) {
    case DrawerPanelVisibility.COLLAPSED:
    case DrawerPanelVisibility.FULLSCREEN:
      return DrawerPanelVisibility.VISIBLE;
    case DrawerPanelVisibility.VISIBLE:
      return DrawerPanelVisibility.COLLAPSED;
    default:
      assertUnreachable(visibility);
  }
}
