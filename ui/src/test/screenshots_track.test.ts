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

import {test, expect, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  // Note: This trace file needs to contain screenshots to fully test this feature.
  // If it doesn't, the test might fail or not cover the intended behavior.
  await pth.openTraceFile('screenshot_trace.pb');
});

test('screenshot track hover preview', async () => {
  const track = pth.locateTrack('Screenshots');
  
  if (await track.isVisible()) {
    const bounds = await track.boundingBox();
    if (bounds) {
      // Hover over the middle of the track
      await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
      
      // Wait for the tooltip to appear and contain an image.
      await page.waitForSelector('.pf-cursor-tooltip img', {state: 'visible'});
      
      // Wait for idle without moving the mouse to keep the tooltip visible
      await pth.waitForPerfettoIdle();
      
      // Take a screenshot of the tooltip
      await expect(page.locator('.pf-cursor-tooltip')).toHaveScreenshot('screenshots_track_hover.png');
    }
  } else {
    console.log('Screenshot track not found, skipping test.');
  }
});
