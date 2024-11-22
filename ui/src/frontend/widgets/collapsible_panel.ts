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
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from '../css_constants';
import {DisposableStack} from '../../base/disposable_stack';
import {DragGestureHandler} from '../../base/drag_gesture_handler';
import {raf} from '../../core/raf_scheduler';
import {assertExists} from '../../base/logging';
import {Button} from '../../widgets/button';

export enum CollapsiblePanelVisibility {
  VISIBLE,
  FULLSCREEN,
  COLLAPSED,
}

export interface CollapsiblePanelAttrs {
  visibility: CollapsiblePanelVisibility;
  setVisibility: (visibility: CollapsiblePanelVisibility) => void;
  headerActions?: m.Children;
  tabs?: m.Children;
}

export class CollapsiblePanel
  implements m.ClassComponent<CollapsiblePanelAttrs>
{
  // The actual height of the vdom node. It matches resizableHeight if VISIBLE,
  // 0 if COLLAPSED, fullscreenHeight if FULLSCREEN.
  private height = 0;

  // The height when the panel is 'VISIBLE'.
  private resizableHeight = getDefaultDetailsHeight();

  // The height when the panel is 'FULLSCREEN'.
  private fullscreenHeight = 0;

  private trash = new DisposableStack();

  view({attrs}: m.CVnode<CollapsiblePanelAttrs>) {
    switch (attrs.visibility) {
      case CollapsiblePanelVisibility.VISIBLE:
        this.height = Math.min(
          Math.max(this.resizableHeight, 0),
          this.fullscreenHeight,
        );
        break;
      case CollapsiblePanelVisibility.FULLSCREEN:
        this.height = this.fullscreenHeight;
        break;
      case CollapsiblePanelVisibility.COLLAPSED:
        this.height = 0;
        break;
    }

    return m(
      '.collapsible-panel',
      m(
        '.handle',
        attrs.headerActions,
        this.renderTabResizeButtons(attrs.visibility, attrs.setVisibility),
      ),
      m(
        '.details-panel-container',
        {
          style: {height: `${this.height}px`},
        },
        attrs.tabs,
      ),
    );
  }

  updatePanelVisibility(
    visibility: CollapsiblePanelVisibility,
    setVisibility: (visibility: CollapsiblePanelVisibility) => void,
  ) {
    setVisibility(visibility);
    raf.scheduleFullRedraw();
  }

  oncreate(vnode: m.VnodeDOM<CollapsiblePanelAttrs, this>) {
    let dragStartY = 0;
    let heightWhenDragStarted = 0;

    this.trash.use(
      new DragGestureHandler(
        vnode.dom as HTMLElement,
        /* onDrag */ (_x, y) => {
          const deltaYSinceDragStart = dragStartY - y;
          this.resizableHeight = heightWhenDragStarted + deltaYSinceDragStart;
          raf.scheduleFullRedraw('force');
        },
        /* onDragStarted */ (_x, y) => {
          this.resizableHeight = this.height;
          heightWhenDragStarted = this.height;
          dragStartY = y;
          vnode.attrs.setVisibility(CollapsiblePanelVisibility.VISIBLE);
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

  private renderTabResizeButtons(
    visibility: CollapsiblePanelVisibility,
    setVisibility: (visibility: CollapsiblePanelVisibility) => void,
  ): m.Child {
    const isClosed = visibility === CollapsiblePanelVisibility.COLLAPSED;
    return m(
      '.buttons',
      m(Button, {
        title: 'Open fullscreen',
        disabled: visibility === CollapsiblePanelVisibility.FULLSCREEN,
        icon: 'vertical_align_top',
        compact: true,
        onclick: () => {
          this.updatePanelVisibility(
            CollapsiblePanelVisibility.FULLSCREEN,
            setVisibility,
          );
        },
      }),
      m(Button, {
        onclick: () => {
          toggleVisibility(visibility, setVisibility);
        },
        title: isClosed ? 'Show panel' : 'Hide panel',
        icon: isClosed ? 'keyboard_arrow_up' : 'keyboard_arrow_down',
        compact: true,
      }),
    );
  }
}

export function toggleVisibility(
  visibility: CollapsiblePanelVisibility,
  setVisibility: (visibility: CollapsiblePanelVisibility) => void,
) {
  switch (visibility) {
    case CollapsiblePanelVisibility.COLLAPSED:
    case CollapsiblePanelVisibility.FULLSCREEN:
      setVisibility(CollapsiblePanelVisibility.VISIBLE);
      break;
    case CollapsiblePanelVisibility.VISIBLE:
      setVisibility(CollapsiblePanelVisibility.COLLAPSED);
      break;
  }

  raf.scheduleFullRedraw();
}

function getDefaultDetailsHeight() {
  const DRAG_HANDLE_HEIGHT_PX = 28;
  // This needs to be a function instead of a const to ensure the CSS constants
  // have been initialized by the time we perform this calculation;
  return DRAG_HANDLE_HEIGHT_PX + DEFAULT_DETAILS_CONTENT_HEIGHT;
}
