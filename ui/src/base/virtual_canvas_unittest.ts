// Copyright (C) 2026 The Android Open Source Project
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

import {VirtualCanvas} from './virtual_canvas';

describe('VirtualCanvas', () => {
  let virtualCanvas: VirtualCanvas;
  let mediaQuery: EventTarget;
  const matchMedia = vi.fn((_query: string) => mediaQuery);
  const onResize = vi.fn();
  const onLayoutShift = vi.fn();

  beforeEach(() => {
    mediaQuery = new EventTarget();
    matchMedia.mockClear();
    vi.stubGlobal('matchMedia', matchMedia);
    vi.stubGlobal('devicePixelRatio', 1);

    const container = document.createElement('div');
    const target = document.createElement('div');
    container.appendChild(target);
    const rect = new DOMRect(0, 0, 300, 200);
    vi.spyOn(container, 'getBoundingClientRect').mockReturnValue(rect);
    vi.spyOn(target, 'getBoundingClientRect').mockReturnValue(rect);
    virtualCanvas = new VirtualCanvas(target, container);
    virtualCanvas.setCanvasResizeListener(onResize);
    virtualCanvas.setLayoutShiftListener(onLayoutShift);
    container.dispatchEvent(new Event('scroll'));
    onResize.mockClear();
    onLayoutShift.mockClear();
  });

  afterEach(() => {
    virtualCanvas[Symbol.dispose]();
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  function changeDpr(dpr: number) {
    const previousQuery = mediaQuery;
    mediaQuery = new EventTarget();
    vi.stubGlobal('devicePixelRatio', dpr);
    previousQuery.dispatchEvent(new Event('change'));
  }

  it('resizes and redraws after each DPR change without a layout change', () => {
    const canvas = virtualCanvas.canvasElement;
    const rect = virtualCanvas.canvasRect;
    for (const dpr of [2, 3, 1]) {
      changeDpr(dpr);

      expect(onResize).toHaveBeenCalledExactlyOnceWith(canvas, 300, 200);
      expect(onLayoutShift).toHaveBeenCalledExactlyOnceWith(canvas, rect);
      expect(canvas.style.width).toBe('300px');
      expect(canvas.style.height).toBe('200px');
      expect(matchMedia).toHaveBeenLastCalledWith(`(resolution: ${dpr}dppx)`);
      onResize.mockClear();
      onLayoutShift.mockClear();
    }
  });

  it('removes resolution listeners when rearming and disposing', () => {
    const previousQuery = mediaQuery;
    changeDpr(2);
    matchMedia.mockClear();
    onResize.mockClear();
    onLayoutShift.mockClear();

    previousQuery.dispatchEvent(new Event('change'));
    expect(matchMedia).not.toHaveBeenCalled();

    virtualCanvas[Symbol.dispose]();
    changeDpr(3);
    expect(matchMedia).not.toHaveBeenCalled();
    expect(onResize).not.toHaveBeenCalled();
    expect(onLayoutShift).not.toHaveBeenCalled();
  });
});
