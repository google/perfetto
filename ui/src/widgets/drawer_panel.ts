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

export enum DrawerPanelVisibility {
  VISIBLE,
  FULLSCREEN,
  COLLAPSED,
}

export interface DrawerPanelAttrs {
  // Content to display in the main area (above the drawer).
  readonly mainContent?: m.Children;

  // Content to display in the handle bar (e.g., tabs, buttons, labels).
  readonly handleContent?: m.Children;

  // Content to display inside the drawer.
  readonly drawerContent?: m.Children;

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
 * collapsible drawer at the bottom.
 *
 * The drawer features a handle that can be dragged to adjust the height of the
 * drawer, and also features buttons to maximize and minimize the drawer.
 *
 * The layout looks like this:
 *
 * ┌───────────────────────────────────────────────────────────────────┐
 * │pf-drawer-panel                                                    │
 * │┌─────────────────────────────────────────────────────────────────┐│
 * ││pf-drawer-panel__main                                            ││
 * │└─────────────────────────────────────────────────────────────────┘│
 * │┌─────────────────────────────────────────────────────────────────┐│
 * ││pf-drawer-panel__handle                                          ││
 * ││┌───────────────────────────────────────┐┌──────────────────────┐││
 * │││handleContent                          ││.pf-button-bar        │││
 * ││└───────────────────────────────────────┘└──────────────────────┘││
 * │└─────────────────────────────────────────────────────────────────┘│
 * │┌─────────────────────────────────────────────────────────────────┐│
 * ││pf-drawer-panel__drawer                                          ││
 * │└─────────────────────────────────────────────────────────────────┘│
 * └───────────────────────────────────────────────────────────────────┘
 */
export class DrawerPanel implements m.ClassComponent<DrawerPanelAttrs> {
  private readonly trash = new DisposableStack();

  // The actual height of the vdom node. It matches resizableHeight if VISIBLE,
  // 0 if COLLAPSED, fullscreenHeight if FULLSCREEN.
  private height = 0;

  // The height when the panel is 'VISIBLE'.
  private resizableHeight: number;

  // The height when the panel is 'FULLSCREEN'.
  private fullscreenHeight = 0;

  // Current visibility state (if not controlled).
  private visibility = DrawerPanelVisibility.VISIBLE;

  constructor({attrs}: m.CVnode<DrawerPanelAttrs>) {
    this.resizableHeight = attrs.startingHeight ?? 100;
  }

  view({attrs}: m.CVnode<DrawerPanelAttrs>) {
    const {
      mainContent,
      handleContent,
      drawerContent,
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

    return m(
      '.pf-drawer-panel',
      {className},
      m('.pf-drawer-panel__main', mainContent),
      m('.pf-drawer-panel__handle', [
        handleContent,
        this.renderResizeButtons(visibility, onVisibilityChange),
      ]),
      m(
        '.pf-drawer-panel__drawer',
        {style: {height: `${this.height}px`}},
        drawerContent,
      ),
    );
  }

  oncreate(vnode: m.VnodeDOM<DrawerPanelAttrs, this>) {
    let dragStartY = 0;
    let heightWhenDragStarted = 0;

    const handle = toHTMLElement(
      assertExists(vnode.dom.querySelector('.pf-drawer-panel__handle')),
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
            DrawerPanelVisibility.VISIBLE,
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

  private renderResizeButtons(
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
