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
import {Portal} from './portal';
import {bindEventListener} from '../base/dom_utils';
import {DisposableStack} from '../base/disposable_stack';
import {HTMLAttrs} from './common';
import {
  createPopper,
  Instance as PopperInstance,
  VirtualElement,
} from '@popperjs/core';
import {classNames} from '../base/classnames';

export interface CursorTooltipAttrs extends HTMLAttrs {}

class VElement implements VirtualElement {
  private x = 0;
  private y = 0;

  getBoundingClientRect() {
    return new DOMRect(this.x, this.y, 0, 0);
  }

  setPosition(x: number, y: number) {
    this.x = x;
    this.y = y;
  }
}

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
        onContentMount: (portal) => {
          this.popper = createPopper(this.virtualElement, portal, {
            placement: 'right-start',
            modifiers: [
              {
                name: 'offset',
                options: {
                  offset: [8, 8], // Shift away from cursor
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
        this.virtualElement.setPosition(e.clientX, e.clientY);
        this.popper?.update();
      }),
    );
  }

  onremove(_: m.VnodeDOM<CursorTooltipAttrs, this>) {
    this.trash.dispose();
  }
}
