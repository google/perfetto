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
  await pth.openTraceFile('json_sort_index_test.json');
});

test('load trace with sort_index metadata', async () => {
  await pth.waitForIdleAndScreenshot('loaded.png');
});

test('verify process sort order', async () => {
  // Processes should be ordered by sort_index (highest first):
  // HighPriorityProcess (100, sort_index=10)
  // MediumPriorityProcess (200, sort_index=5)
  // LowPriorityProcess (300, sort_index=1)

  const highPriorityProcess = pth.locateTrack('HighPriorityProcess 100');
  await highPriorityProcess.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('process_order.png');
});

test('verify thread sort order within process', async () => {
  // Expand the HighPriorityProcess to see threads
  const highPriorityProcess = pth.locateTrack('HighPriorityProcess 100');
  await highPriorityProcess.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(highPriorityProcess);

  // Threads should be ordered by sort_index (highest first):
  // HighPriorityThread (101, sort_index=100)
  // MediumPriorityThread (102, sort_index=50)
  // LowPriorityThread (103, sort_index=10)

  await pth.waitForIdleAndScreenshot('thread_order.png');
});
