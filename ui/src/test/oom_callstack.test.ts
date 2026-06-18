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
  await pth.openTraceFile('oom_callstack.pftrace');
});

test('OOM callstack track', async () => {
  const processGrp = pth.locateTrack('com.example.oometest 12345');
  await processGrp.scrollIntoViewIfNeeded();
  const expandBtn = processGrp.locator('button', {hasText: 'expand_more'});
  if ((await expandBtn.count()) > 0) {
    await expandBtn.click();
    await pth.waitForPerfettoIdle();
  }

  const oomTrack = pth.locateTrack(
    'com.example.oometest 12345/OOM Callstack',
    processGrp,
  );
  await oomTrack.scrollIntoViewIfNeeded();

  await pth.waitForIdleAndScreenshot('oom_callstack.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});
