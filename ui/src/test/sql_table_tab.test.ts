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

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
});

test('slices with same name', async () => {
  const sliceName = 'animation';
  await pth.searchSlice(sliceName);
  await page
    .locator('.details-panel-container a.pf-anchor', {hasText: sliceName})
    .click();
  await page
    .locator('.pf-popup-portal button', {hasText: 'Slices with the same name'})
    .click();
  await pth.waitForIdleAndScreenshot(`slices-with-same-name.png`);
});

test('ShowTable command', async () => {
  await pth.runCommand('perfetto.ShowTable.slice');
  await pth.waitForIdleAndScreenshot(`slices-table.png`);
});
