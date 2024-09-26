// Copyright (C) 2024 The Android Open Source Project
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
import {assertExists} from '../base/logging';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

// Clip only the bottom half of the UI. When dealing with area selection, the
// time-width of the mouse-based region (which then is showed up in the upper
// ruler) is not 100% reproducible.
const SCREEN_CLIP = {
  clip: {
    x: 230,
    y: 500,
    width: 1920,
    height: 1080,
  },
};

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('wattson_dsu_pmu.pb', {
    enablePlugins: 'org.kernel.Wattson',
  });
});

test('wattson aggregations', async () => {
  const wattsonGrp = pth.locateTrackGroup('Wattson');
  await wattsonGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(wattsonGrp);
  const cpuEstimate = pth.locateTrack('Wattson/Cpu0 Estimate', wattsonGrp);
  const coords = assertExists(await cpuEstimate.boundingBox());
  await page.keyboard.press('Escape');
  await page.mouse.move(600, coords.y + 10);
  await page.mouse.down();
  await page.mouse.move(1000, coords.y + 80);
  await page.mouse.up();
  await pth.waitForIdleAndScreenshot('wattson-estimate-aggr.png', SCREEN_CLIP);
  await page.keyboard.press('Escape');
});

test('sched aggregations', async () => {
  await page.keyboard.press('Escape');
  await page.mouse.move(600, 250);
  await page.mouse.down();
  await page.mouse.move(800, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();

  await page.click('button[label="Wattson by thread"]');
  await pth.waitForIdleAndScreenshot('sched-aggr-thread.png', SCREEN_CLIP);

  await page.click('button[label="Wattson by process"]');
  await pth.waitForIdleAndScreenshot('sched-aggr-process.png', SCREEN_CLIP);

  await page.keyboard.press('Escape');
});
