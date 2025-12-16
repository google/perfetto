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
import {HTMLAttrs} from './common';
import {MithrilEvent} from '../base/mithril_utils';

export interface ResizeHandleAttrs extends HTMLAttrs {
  onResize(deltaPx: number): void;
  onResizeStart?(): void;
  onResizeEnd?(): void;
  // Direction of the resize handle:
  // - 'vertical' (default): horizontal bar that can be dragged up/down
  // - 'horizontal': vertical bar that can be dragged left/right
  direction?: 'vertical' | 'horizontal';
}

export class ResizeHandle implements m.ClassComponent<ResizeHandleAttrs> {
  private handleElement?: HTMLElement;
  private previousY: number | undefined;
  private previousX: number | undefined;

  oncreate(vnode: m.VnodeDOM<ResizeHandleAttrs, this>) {
    this.handleElement = vnode.dom as HTMLElement;
  }

  private endDrag(attrs: ResizeHandleAttrs, pointerId: number) {
    if (this.previousY !== undefined || this.previousX !== undefined) {
      this.previousY = undefined;
      this.previousX = undefined;
      this.handleElement!.releasePointerCapture(pointerId);
      attrs.onResizeEnd?.();
    }
  }

  view({attrs}: m.CVnode<ResizeHandleAttrs>): m.Children {
    const {
      onResize: _onResize,
      onResizeStart: _onResizeStart,
      onResizeEnd: _onResizeEnd,
      direction = 'vertical',
      ...rest
    } = attrs;

    const isHorizontal = direction === 'horizontal';

    return m('.pf-resize-handle', {
      class: isHorizontal ? 'pf-resize-handle--horizontal' : '',
      oncontextmenu: (e: Event) => {
        e.preventDefault();
      },
      onpointerdown: (e: PointerEvent) => {
        const offsetParent = this.handleElement?.offsetParent as HTMLElement;

        if (isHorizontal) {
          const offsetLeft = offsetParent?.getBoundingClientRect().left ?? 0;
          const mouseOffsetX = e.clientX - offsetLeft;
          this.previousX = mouseOffsetX;
        } else {
          const offsetTop = offsetParent?.getBoundingClientRect().top ?? 0;
          const mouseOffsetY = e.clientY - offsetTop;
          this.previousY = mouseOffsetY;
        }

        this.handleElement!.setPointerCapture(e.pointerId);
        attrs.onResizeStart?.();
      },
      onpointermove: (e: MithrilEvent<PointerEvent>) => {
        const offsetParent = this.handleElement?.offsetParent as HTMLElement;

        // We typically just resize some element when dragging the handle, so we
        // tell Mithril not to redraw after this event.
        e.redraw = false;

        // Note: We don't check hasPointerCapture() here because pointer capture
        // already ensures we only receive move events during an active drag.
        // The previousX/previousY check is sufficient to determine drag state.

        if (isHorizontal) {
          const offsetLeft = offsetParent?.getBoundingClientRect().left ?? 0;
          const mouseOffsetX = e.clientX - offsetLeft;

          if (this.previousX !== undefined) {
            attrs.onResize(mouseOffsetX - this.previousX);
            this.previousX = mouseOffsetX;
          }
        } else {
          const offsetTop = offsetParent?.getBoundingClientRect().top ?? 0;
          const mouseOffsetY = e.clientY - offsetTop;

          if (this.previousY !== undefined) {
            attrs.onResize(mouseOffsetY - this.previousY);
            this.previousY = mouseOffsetY;
          }
        }
      },
      onpointerup: (e: PointerEvent) => {
        this.endDrag(attrs, e.pointerId);
      },
      onpointercancel: (e: PointerEvent) => {
        this.endDrag(attrs, e.pointerId);
      },
      onpointercapturelost: (e: PointerEvent) => {
        this.endDrag(attrs, e.pointerId);
      },
      ...rest,
    });
  }
}
