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

import {test, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('heap_profile_intervals.pftrace');
});

// The trace has three back-to-back native heap dumps carrying start_timestamps,
// so each renders as a windowed interval rather than an instant.
test('native heap profile interval track', async () => {
  await page.click('a[href="#!/viewer"]');
  await pth.waitForPerfettoIdle();

  const processGrp = pth.locateTrack('com.example.heapprofd 1234');
  await processGrp.scrollIntoViewIfNeeded();
  const expandBtn = processGrp.locator('button', {hasText: 'expand_more'});
  if ((await expandBtn.count()) > 0) {
    await expandBtn.click();
    await pth.waitForPerfettoIdle();
  }

  const heapTrack = pth.locateTrack(
    'com.example.heapprofd 1234/Native heap profile',
    processGrp,
  );
  await heapTrack.scrollIntoViewIfNeeded();

  await pth.waitForIdleAndScreenshot('heap_profile_intervals.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});
