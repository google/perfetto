// Copyright (C) 2023 The Android Open Source Project
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

export type RedrawCallback = () => void;

export interface Raf {
  /**
   * Schedule both a DOM and canvas redraw.
   */
  scheduleFullRedraw(): void;

  /**
   * Schedule a canvas redraw only.
   */
  scheduleCanvasRedraw(): void;

  /**
   * Add a callback for canvas redraws. `cb` will be called whenever a canvas
   * redraw is scheduled canvas redraw using {@link scheduleCanvasRedraw()}.
   *
   * @param cb - The callback to called when canvas are redrawn.
   * @returns - A disposable object that removes the callback when disposed.
   */
  addCanvasRedrawCallback(cb: RedrawCallback): Disposable;
}
