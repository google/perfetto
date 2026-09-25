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
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
});

test('pin actual timeline tracks', async () => {
  const sfGroup = pth.locateTrack('/system/bin/surfaceflinger 598');
  await sfGroup.scrollIntoViewIfNeeded();
  await pth.expandTrackGroup(sfGroup);
  const sfTrack = pth.locateTrack(
    '/system/bin/surfaceflinger 598/Actual Timeline',
    sfGroup,
  );
  await pth.pinTrackUsingShellBtn(sfTrack);
  await pth.waitForPerfettoIdle();

  const sysuiGroup = pth.locateTrack('com.android.systemui 25348');
  await sysuiGroup.scrollIntoViewIfNeeded();
  await pth.expandTrackGroup(sysuiGroup);
  const sysuiTrack = pth.locateTrack(
    'com.android.systemui 25348/Actual Timeline',
    sysuiGroup,
  );
  await pth.pinTrackUsingShellBtn(sysuiTrack);
  await pth.waitForPerfettoIdle();

  await pth.waitForIdleAndScreenshot('pinned_actual_timelines.png', {
    locator: page.locator('.pf-timeline-page__pinned-track-tree'),
  });
});
