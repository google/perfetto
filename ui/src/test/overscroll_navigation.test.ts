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

import {chromium, expect, test} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

interface GpuBenchmarking {
  readonly MOUSE_INPUT: number;
  readonly TOUCH_INPUT: number;
  smoothScrollByXY(
    pixelsToScrollX: number,
    pixelsToScrollY: number,
    callback: () => void,
    startX: number,
    startY: number,
    gestureSourceType: number,
    speedInPixelsS: number,
  ): void;
}

interface WindowWithGpuBenchmarking {
  readonly chrome?: {
    readonly gpuBenchmarking?: GpuBenchmarking;
  };
}

test('two-finger horizontal scroll does not trigger browser back navigation', async () => {
  const browser = await chromium.launch({
    channel: 'chromium',
    args: [
      '--enable-gpu-benchmarking',
      '--overscroll-history-navigation=1',
      '--enable-features=TouchpadOverscrollHistoryNavigation',
    ],
  });

  try {
    const context = await browser.newContext({
      viewport: {width: 1920, height: 1080},
    });
    const page = await context.newPage();

    // 1. Navigate to an initial URL first so the tab has a Back history entry.
    await page.goto('about:blank');

    // 2. Open Perfetto and load a trace.
    const pth = new PerfettoTestHelper(page);
    await pth.openTraceFile('api34_startup_cold.perfetto-trace');
    const traceUrl = page.url();

    // 3. Perform a two-finger horizontal swipe right (negative X scroll delta)
    // beyond the 30% viewport width threshold (0.3 * 1920 = 576px) using
    // chrome.gpuBenchmarking.smoothScrollByXY.
    await page.evaluate(() => {
      return new Promise<void>((resolve) => {
        const win = window as unknown as WindowWithGpuBenchmarking;
        const benchmarking = win.chrome?.gpuBenchmarking;
        if (benchmarking === undefined) {
          throw new Error(
            'chrome.gpuBenchmarking is unavailable; ensure --enable-gpu-benchmarking is passed',
          );
        }
        benchmarking.smoothScrollByXY(
          -1000,
          0,
          () => resolve(),
          500,
          500,
          benchmarking.MOUSE_INPUT,
          2500,
        );
      });
    });

    // 4. In Chromium's OverscrollController::DispatchEventCompletesAction,
    // a touchpad overscroll waits for momentum end or a MouseMove event once
    // past the completion threshold before calling CompleteAction().
    // Moving the mouse releases/completes the gesture.
    await page.mouse.move(501, 500);
    await page.waitForTimeout(500);

    // 5. Verify the browser stayed on the trace and did not navigate back.
    expect(page.url()).toBe(traceUrl);
  } finally {
    await browser.close();
  }
});
