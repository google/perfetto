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

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
});

test('sched', async () => {
  await page.mouse.move(600, 250);
  await page.mouse.down();
  await page.mouse.move(800, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('cpu-by-thread.png');

  await page.click('button[label="CPU by process"]');
  await pth.waitForIdleAndScreenshot('cpu-by-process.png');

  // Now test sorting.

  const hdr = page.getByRole('cell', {name: 'Avg Wall duration (ms)'});
  await hdr.click();
  await pth.waitForIdleAndScreenshot('sort-by-wall-duration.png');

  await hdr.click();
  await pth.waitForIdleAndScreenshot('sort-by-wall-duration-desc.png');

  await page.getByRole('cell', {name: 'Occurrences'}).click();
  await pth.waitForIdleAndScreenshot('sort-by-occurrences.png');
});

test('gpu counter', async () => {
  await page.keyboard.press('Escape');
  const gpuTrack = pth.locateTrack('Gpu 0 Frequency');
  const coords = assertExists(await gpuTrack.boundingBox());
  await page.mouse.move(600, coords.y + 10);
  await page.mouse.down();
  await page.mouse.move(800, coords.y + 60);
  await page.mouse.up();
  await pth.waitForIdleAndScreenshot('gpu-counter-aggregation.png');
});

test('frametimeline', async () => {
  await page.keyboard.press('Escape');
  const sysui = pth.locateTrackGroup('com.android.systemui 25348');
  await sysui.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(sysui);
  const actualTimeline = pth.locateTrack(
    'com.android.systemui 25348/Actual Timeline',
    sysui,
  );
  const coords = assertExists(await actualTimeline.boundingBox());
  await page.mouse.move(600, coords.y + 10);
  await page.mouse.down();
  await page.mouse.move(1000, coords.y + 20);
  await page.mouse.up();
  await pth.waitForIdleAndScreenshot('frame-timeline-aggregation.png');
});

test('slices', async () => {
  await page.keyboard.press('Escape');
  const syssrv = pth.locateTrackGroup('system_server 1719');
  await syssrv.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(syssrv);
  const animThread = pth
    .locateTrack('system_server 1719/android.anim 1754', syssrv)
    .nth(1);
  await animThread.scrollIntoViewIfNeeded();
  await pth.waitForPerfettoIdle();
  const coords = assertExists(await animThread.boundingBox());
  await page.mouse.move(600, coords.y + 10);
  await page.mouse.down();
  await page.mouse.move(1000, coords.y + 20);
  await page.mouse.up();
  await pth.waitForIdleAndScreenshot('slice-aggregation.png');
});
