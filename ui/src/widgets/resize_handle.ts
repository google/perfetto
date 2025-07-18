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
  private dragging = false;

  oncreate(vnode: m.VnodeDOM<ResizeHandleAttrs, this>) {
    this.handleElement = vnode.dom as HTMLElement;
  }

  view({attrs}: m.CVnode<ResizeHandleAttrs>): m.Children {
    const {
      onResize: _onResize,
      onResizeStart: _onResizeStart,
      onResizeEnd: _onResizeEnd,
      ...rest
    } = attrs;

    return m('.pf-resize-handle', {
      onpointerdown: (e: PointerEvent) => {
        this.dragging = true;
        this.handleElement!.setPointerCapture(e.pointerId);
        attrs.onResizeStart?.();
      },
      onpointermove: (e: MithrilEvent<PointerEvent>) => {
        // We typically just resize some element when dragging the handle, so we
        // tell Mithril not to redraw after this event.
        e.redraw = false;
        if (this.dragging) {
          attrs.onResize(e.movementY);
        }
      },
      onpointerup: (e: PointerEvent) => {
        if (this.dragging) {
          this.dragging = false;
          this.handleElement!.releasePointerCapture(e.pointerId);
          attrs.onResizeEnd?.();
        }
      },
      ...rest,
    });
  }
}
