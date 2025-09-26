// Copyright (C) 2025 The Android Open Source Project
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

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('perf_sample_multicallstacks.pftrace');
});

test('multiple callstack tracks', async () => {
  const grp = pth.locateTrack('surfaceflinger 558');
  await grp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(grp);

  await pth.waitForIdleAndScreenshot('perf_event_sf.png');

  const processGrp = pth.locateTrack(
    'surfaceflinger 558/Process callstacks',
    grp,
  );
  await processGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(processGrp);
  const threadGrp = pth.locateTrack(
    'surfaceflinger 558/Thread 558 callstacks',
    grp,
  );
  await threadGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(threadGrp);

  await pth.waitForIdleAndScreenshot('perf_event_sf_expanded.png');
});
