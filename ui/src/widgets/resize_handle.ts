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
}

export class ResizeHandle implements m.ClassComponent<ResizeHandleAttrs> {
  private handleElement?: HTMLElement;
  private previousY: number | undefined;

  oncreate(vnode: m.VnodeDOM<ResizeHandleAttrs, this>) {
    this.handleElement = vnode.dom as HTMLElement;
  }

  private endDrag(attrs: ResizeHandleAttrs, pointerId: number) {
    if (this.previousY !== undefined) {
      this.previousY = undefined;
      this.handleElement!.releasePointerCapture(pointerId);
      attrs.onResizeEnd?.();
    }
  }

  view({attrs}: m.CVnode<ResizeHandleAttrs>): m.Children {
    const {
      onResize: _onResize,
      onResizeStart: _onResizeStart,
      onResizeEnd: _onResizeEnd,
      ...rest
    } = attrs;

    return m('.pf-resize-handle', {
      oncontextmenu: (e: Event) => {
        e.preventDefault();
      },
      onpointerdown: (e: PointerEvent) => {
        const offsetParent = this.handleElement?.offsetParent as HTMLElement;
        const offsetTop = offsetParent?.getBoundingClientRect().top ?? 0;
        const mouseOffsetY = e.clientY - offsetTop;
        this.previousY = mouseOffsetY;

        this.handleElement!.setPointerCapture(e.pointerId);
        attrs.onResizeStart?.();
      },
      onpointermove: (e: MithrilEvent<PointerEvent>) => {
        const offsetParent = this.handleElement?.offsetParent as HTMLElement;
        const offsetTop = offsetParent?.getBoundingClientRect().top ?? 0;
        const mouseOffsetY = e.clientY - offsetTop;

        // We typically just resize some element when dragging the handle, so we
        // tell Mithril not to redraw after this event.
        e.redraw = false;
        if (
          this.previousY !== undefined
          // && this.handleElement!.hasPointerCapture(e.pointerId)
        ) {
          attrs.onResize(mouseOffsetY - this.previousY);
          this.previousY = mouseOffsetY;
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
