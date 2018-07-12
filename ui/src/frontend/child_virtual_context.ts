// Copyright (C) 2018 The Android Open Source Project
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

import {BoundingRect, VirtualCanvasContext} from './virtual_canvas_context';

/**
 * ChildVirtualContext is a VirtualCanvasContext that is a child of another
 * VirtualCanvasContext. A ChildVirtualContext has a boundingRect within the
 * parent context, and uses this to determine whether it is currently on the
 * canvas, hence disabling unnecessary draw calls. ChildVirtualContexts can be
 * nested, and their bounds are relative to one another.
 */
export class ChildVirtualContext extends VirtualCanvasContext {
  constructor(
      protected parentCtx: VirtualCanvasContext,
      protected boundingRect: BoundingRect) {
    super(parentCtx);
  }

  isOnCanvas() {
    return this.parentCtx.checkRectOnCanvas(this.boundingRect);
  }

  checkRectOnCanvas(boundingRect: BoundingRect): boolean {
    return this.parentCtx.checkRectOnCanvas({
      y: boundingRect.y + this.boundingRect.y,
      x: boundingRect.x + this.boundingRect.x,
      width: boundingRect.width,
      height: boundingRect.height
    });
  }

  getBoundingRect() {
    return this.boundingRect;
  }
}
