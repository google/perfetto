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
import {DisposableStack} from '../../../base/disposable_stack';
import {DragGestureHandler} from '../../../base/drag_gesture_handler';
import {assertExists} from '../../../base/logging';

interface VerticalSplitContainerAttrs {
  leftPane: m.Children;
  rightPane: m.Children;
}

export class VerticalSplitContainer
  implements m.ClassComponent<VerticalSplitContainerAttrs>
{
  // Note: For BEM class names (https://getbem.com/)
  private readonly leftPaneClassName =
    '.pf-vertical-split-container__left-pane';
  private readonly leftPaneResizeHandle =
    this.leftPaneClassName + '__resize-handle';
  private readonly rightPaneClassName =
    '.pf-vertical-split-container__right-pane';

  private readonly trash = new DisposableStack();
  private leftPaneWidth = 0;
  private rightPaneWidth = 0;

  oncreate({dom}: m.VnodeDOM<VerticalSplitContainerAttrs, this>) {
    const leftPane = assertExists(
      dom.querySelector(this.leftPaneClassName),
    ) as HTMLElement;
    const rightPane = assertExists(
      dom.querySelector(this.rightPaneClassName),
    ) as HTMLElement;

    this.trash.use(
      new DragGestureHandler(
        assertExists(
          dom.querySelector(this.leftPaneResizeHandle),
        ) as HTMLElement,
        /* onDrag */
        (x, _y) => {
          leftPane.style.width = `${this.leftPaneWidth + x}px`;
          rightPane.style.width = `${this.rightPaneWidth - x}px`;
        },
        /* onDragStarted */
        () => {
          this.leftPaneWidth = leftPane.clientWidth;
        },
        /* onDragFinished */
        () => {},
      ),
    );
  }

  onremove(): void {
    this.trash.dispose();
  }

  view({attrs}: m.VnodeDOM<VerticalSplitContainerAttrs, this>) {
    return m(
      '.pf-vertical-split-container',
      m(
        this.leftPaneClassName,
        m(this.leftPaneClassName + '__content', attrs.leftPane),
        m(this.leftPaneResizeHandle),
      ),
      m(this.rightPaneClassName, attrs.rightPane),
    );
  }
}
