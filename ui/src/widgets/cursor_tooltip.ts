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
import {MountOptions, Portal} from './portal';
import {bindEventListener} from '../base/dom_utils';
import {DisposableStack} from '../base/disposable_stack';
import {HTMLAttrs} from './common';
import {
  createPopper,
  Instance as PopperInstance,
  VirtualElement,
} from '@popperjs/core';
import {classNames} from '../base/classnames';
import {Point2D, Vector2D} from '../base/geom';

export interface CursorTooltipAttrs extends HTMLAttrs {}

class VElement implements VirtualElement {
  private pos = new Vector2D({x: 0, y: 0});

  getBoundingClientRect() {
    return new DOMRect(this.pos.x, this.pos.y, 0, 0);
  }

  setPosition(pos: Point2D) {
    this.pos = new Vector2D(pos);
  }
}

// Keep track of the mouse position in the document so that the cursor can be
// initially drawn in the correct place, before it's received any mouse events.
let globalMousePos: Point2D;
document.addEventListener('mousemove', (e) => {
  globalMousePos = new Vector2D({x: e.clientX, y: e.clientY});
});

/**
 * Provides a little tooltip that's permanently attached to the mouse.
 *
 * Any children are rendered inside - the tooltip is displayed to the bottom
 * right if there is room.
 */
export class CursorTooltip implements m.ClassComponent<CursorTooltipAttrs> {
  private readonly trash = new DisposableStack();
  private readonly virtualElement = new VElement();
  private popper?: PopperInstance;

  view({children, attrs}: m.Vnode<CursorTooltipAttrs>) {
    const {className, ...rest} = attrs;
    return m(
      Portal,
      {
        ...rest,
        className: classNames('pf-cursor-tooltip', className),
        onBeforeContentMount: (dom: Element): MountOptions => {
          const closestModal = dom.closest('.pf-overlay-container');
          if (closestModal) {
            return {container: closestModal};
          }
          return {container: undefined};
        },
        onContentMount: (portal) => {
          this.virtualElement.setPosition(globalMousePos);
          this.popper = createPopper(this.virtualElement, portal, {
            placement: 'right',
            modifiers: [
              {
                name: 'offset',
                options: {
                  offset: [0, 8], // Shift away from cursor
                },
              },
            ],
          });
        },
        onContentUnmount: () => {
          this.popper?.destroy();
        },
      },
      children,
    );
  }

  oncreate(_: m.VnodeDOM<CursorTooltipAttrs>) {
    this.trash.use(
      bindEventListener(document, 'mousemove', (e) => {
        this.virtualElement.setPosition({x: e.clientX, y: e.clientY});
        this.popper?.update();
      }),
    );
  }

  onremove(_: m.VnodeDOM<CursorTooltipAttrs, this>) {
    this.trash.dispose();
  }
}
