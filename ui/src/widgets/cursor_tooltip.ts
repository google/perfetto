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
import {type MountOptions, Portal} from './portal';
import {bindEventListener} from '../base/dom_utils';
import {DisposableStack} from '../base/disposable_stack';
import type {HTMLAttrs} from './common';
import {
  createPopper,
  type Instance as PopperInstance,
  type Modifier,
  type OptionsGeneric,
  type VirtualElement,
} from '@popperjs/core';
import {classNames} from '../base/classnames';
import {type Point2D, Vector2D} from '../base/geom';
import {PopupPosition} from './popup';
import type {ExtendedModifiers} from './popper_utils';

export interface CursorTooltipAttrs extends HTMLAttrs {
  // Which side of the cursor to place the tooltip. Defaults to Right.
  readonly position?: PopupPosition;
  // Distance in px between the tooltip and the cursor. Default = 8.
  readonly offset?: number;
}

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
  private tooltipElement?: HTMLElement;
  // An empty element rendered inline in the component tree (as opposed to the
  // tooltip content, which is portalled elsewhere). Its visibility tracks that
  // of the tooltip's parent, so we can hide the tooltip when the parent goes
  // away (e.g. an ancestor gets display:none).
  private canaryElement?: HTMLElement;
  private popper?: PopperInstance;

  view({children, attrs}: m.Vnode<CursorTooltipAttrs>) {
    const {className, ...rest} = attrs;
    return [
      m('.pf-cursor-tooltip-canary', {
        // Take up no space and don't affect layout, but keep a layout box so
        // checkVisibility() reflects our ancestors' visibility.
        style: {position: 'absolute', width: '0', height: '0'},
        oncreate: (v: m.VnodeDOM) => {
          this.canaryElement = v.dom as HTMLElement;
        },
        onremove: () => {
          this.canaryElement = undefined;
        },
      }),
      m(
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
            this.tooltipElement = portal;
            this.virtualElement.setPosition(globalMousePos);
            this.createOrUpdatePopper(attrs);
          },
          onContentUnmount: () => {
            this.popper?.destroy();
            this.popper = undefined;
            this.tooltipElement = undefined;
          },
        },
        children,
      ),
    ];
  }

  oncreate(_: m.VnodeDOM<CursorTooltipAttrs>) {
    this.trash.use(
      bindEventListener(document, 'mousemove', (e) => {
        this.virtualElement.setPosition({x: e.clientX, y: e.clientY});
        this.popper?.update();
      }),
    );
  }

  onupdate({attrs}: m.VnodeDOM<CursorTooltipAttrs, this>) {
    this.createOrUpdatePopper(attrs);
  }

  onremove(_: m.VnodeDOM<CursorTooltipAttrs, this>) {
    this.trash.dispose();
  }

  private createOrUpdatePopper(attrs: CursorTooltipAttrs) {
    const {position = PopupPosition.Right, offset = 8} = attrs;

    // Custom modifier to hide the tooltip when our canary - and hence the
    // tooltip's parent - is not visible. This can be due to the canary or one
    // of its ancestors having display:none.
    const hideOnInvisible: Modifier<'hideOnInvisible', {}> = {
      name: 'hideOnInvisible',
      enabled: true,
      phase: 'main',
      fn: ({state}) => {
        const el = this.canaryElement;
        if (el === undefined) {
          return;
        }
        const isVisible =
          typeof el.checkVisibility === 'function'
            ? el.checkVisibility()
            : window.getComputedStyle(el).display !== 'none' &&
              window.getComputedStyle(el).visibility !== 'hidden';
        state.elements.popper.style.display = isVisible ? '' : 'none';
      },
    };

    const options: Partial<OptionsGeneric<ExtendedModifiers>> = {
      placement: position,
      modifiers: [
        {
          name: 'offset',
          options: {
            offset: [0, offset], // Shift away from cursor
          },
        },
        hideOnInvisible,
      ],
    };

    if (this.popper) {
      this.popper.setOptions(options);
    } else if (this.tooltipElement) {
      this.popper = createPopper<ExtendedModifiers>(
        this.virtualElement,
        this.tooltipElement,
        options,
      );
    }
  }
}
