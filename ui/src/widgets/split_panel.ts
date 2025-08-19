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
import {DisposableStack} from '../base/disposable_stack';
import {toHTMLElement} from '../base/dom_utils';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {assertExists, assertUnreachable} from '../base/logging';
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
      '.pf-split-panel__tab',
      {
        ...rest,
        className: classNames(active && 'pf-split-panel__tab--active'),
        onauxclick: () => {
          attrs.onClose?.();
        },
      },
      m('.pf-split-panel__tab-title', children),
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

export enum SplitPanelDrawerVisibility {
  VISIBLE,
  FULLSCREEN,
  COLLAPSED,
}

export interface TabbedSplitPanelAttrs {
  // Content to put to the left of the tabs on the split handle.
  readonly leftHandleContent?: m.Children;

  // Tabs to display on the split handle.
  readonly tabs?: m.Children;

  // Content to display inside the drawer.
  readonly drawerContent?: m.Children;

  // Whether the drawer is currently visible or not (when in controlled mode).
  readonly visibility?: SplitPanelDrawerVisibility;

  // Extra classes applied to the root element.
  readonly className?: string;

  // What height should the drawer be initially?
  readonly startingHeight?: number;

  // Called when the active tab is changed.
  onTabChange?(key: string): void;

  // Called when the drawer visibility is changed.
  onVisibilityChange?(visibility: SplitPanelDrawerVisibility): void;
}

/**
 * A container that fills its parent container, splitting into two adjustable
 * horizontal sections. The upper half is reserved for the main content and any
 * children are placed here, and the lower half should be considered a drawer,
 * the `drawerContent` attribute can be used to define what goes here.
 *
 * The drawer features a handle that can be dragged to adjust the height of the
 * drawer, and also features buttons to maximize and minimise the drawer.
 *
 * Content can also optionally be displayed on the handle itself to the left of
 * the buttons.
 *
 * The layout looks like this:
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │pf-split-panel                                                    │
 * │┌────────────────────────────────────────────────────────────────┐|
 * ││pf-split-panel__main                                            ││
 * |└────────────────────────────────────────────────────────────────┘|
 * │┌────────────────────────────────────────────────────────────────┐|
 * ││pf-split-panel__handle                                          ││
 * │|┌─────────────────┐┌─────────────────────┐┌────────────────────┐||
 * |||leftHandleContent||.pf-split-panel__tabs||.pf-button-bar      |||
 * ||└─────────────────┘└─────────────────────┘└────────────────────┘||
 * |└────────────────────────────────────────────────────────────────┘|
 * │┌────────────────────────────────────────────────────────────────┐|
 * ││pf-split-panel__drawer                                          ││
 * |└────────────────────────────────────────────────────────────────┘|
 * └──────────────────────────────────────────────────────────────────┘
 */
export class SplitPanel implements m.ClassComponent<TabbedSplitPanelAttrs> {
  private readonly trash = new DisposableStack();

  // The actual height of the vdom node. It matches resizableHeight if VISIBLE,
  // 0 if COLLAPSED, fullscreenHeight if FULLSCREEN.
  private height = 0;

  // The height when the panel is 'VISIBLE'.
  private resizableHeight: number;

  // The height when the panel is 'FULLSCREEN'.
  private fullscreenHeight = 0;

  // Current visibility state (if not controlled).
  private visibility = SplitPanelDrawerVisibility.VISIBLE;

  constructor({attrs}: m.CVnode<TabbedSplitPanelAttrs>) {
    this.resizableHeight = attrs.startingHeight ?? 100;
  }

  view({attrs, children}: m.CVnode<TabbedSplitPanelAttrs>) {
    const {
      leftHandleContent,
      drawerContent,
      visibility = this.visibility,
      className,
      onVisibilityChange,
      tabs,
    } = attrs;

    switch (visibility) {
      case SplitPanelDrawerVisibility.VISIBLE:
        this.height = Math.min(
          Math.max(this.resizableHeight, 0),
          this.fullscreenHeight,
        );
        break;
      case SplitPanelDrawerVisibility.FULLSCREEN:
        this.height = this.fullscreenHeight;
        break;
      case SplitPanelDrawerVisibility.COLLAPSED:
        this.height = 0;
        break;
    }

    return m(
      '.pf-split-panel',
      {
        className,
      },
      m('.pf-split-panel__main', children),
      m('.pf-split-panel__handle', [
        leftHandleContent,
        m('.pf-split-panel__tabs', tabs),
        this.renderTabResizeButtons(visibility, onVisibilityChange),
      ]),
      m(
        '.pf-split-panel__drawer',
        {
          style: {height: `${this.height}px`},
        },
        drawerContent,
      ),
    );
  }

  oncreate(vnode: m.VnodeDOM<TabbedSplitPanelAttrs, this>) {
    let dragStartY = 0;
    let heightWhenDragStarted = 0;

    const handle = toHTMLElement(
      assertExists(vnode.dom.querySelector('.pf-split-panel__handle')),
    );

    this.trash.use(
      new DragGestureHandler(
        handle,
        /* onDrag */ (_x, y) => {
          const deltaYSinceDragStart = dragStartY - y;
          this.resizableHeight = heightWhenDragStarted + deltaYSinceDragStart;
          m.redraw();
        },
        /* onDragStarted */ (_x, y) => {
          this.resizableHeight = this.height;
          heightWhenDragStarted = this.height;
          dragStartY = y;
          this.updatePanelVisibility(
            SplitPanelDrawerVisibility.VISIBLE,
            vnode.attrs.onVisibilityChange,
          );
        },
        /* onDragFinished */ () => {},
      ),
    );

    const parent = assertExists(vnode.dom.parentElement);
    this.fullscreenHeight = parent.clientHeight;
    const resizeObs = new ResizeObserver(() => {
      this.fullscreenHeight = parent.clientHeight;
      m.redraw();
    });
    resizeObs.observe(parent);
    this.trash.defer(() => resizeObs.disconnect());
  }

  onremove() {
    this.trash.dispose();
  }

  private renderTabResizeButtons(
    visibility: SplitPanelDrawerVisibility,
    setVisibility?: (visibility: SplitPanelDrawerVisibility) => void,
  ): m.Child {
    const isClosed = visibility === SplitPanelDrawerVisibility.COLLAPSED;
    return m(
      ButtonBar,
      m(Button, {
        title: 'Open fullscreen',
        disabled: visibility === SplitPanelDrawerVisibility.FULLSCREEN,
        icon: 'vertical_align_top',
        onclick: () => {
          this.updatePanelVisibility(
            SplitPanelDrawerVisibility.FULLSCREEN,
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
    visibility: SplitPanelDrawerVisibility,
    setVisibility?: (visibility: SplitPanelDrawerVisibility) => void,
  ) {
    this.visibility = visibility;
    setVisibility?.(visibility);
  }
}

export function toggleVisibility(visibility: SplitPanelDrawerVisibility) {
  switch (visibility) {
    case SplitPanelDrawerVisibility.COLLAPSED:
    case SplitPanelDrawerVisibility.FULLSCREEN:
      return SplitPanelDrawerVisibility.VISIBLE;
    case SplitPanelDrawerVisibility.VISIBLE:
      return SplitPanelDrawerVisibility.COLLAPSED;
    default:
      assertUnreachable(visibility);
  }
}
