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

import {test, Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

/**
 * This test verifies that the Canvas 2D renderer fallback works correctly
 * when WebGL is disabled. It loads a trace with WebGL rendering disabled
 * and captures a screenshot to verify the canvas renderer is functioning.
 */
test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);

  // Disable WebGL rendering via feature flags before loading the trace.
  // This forces the UI to use the Canvas 2D renderer fallback.
  await page.goto('/?testing=1');
  await page.evaluate(() => {
    localStorage.setItem(
      'perfettoFeatureFlags',
      JSON.stringify({webglRendering: 'OVERRIDE_FALSE'}),
    );
  });

  await pth.openTraceFile('synth_1.pb');
});

test('load trace with canvas renderer', async () => {
  await pth.waitForIdleAndScreenshot('canvas_renderer_loaded.png');
});
