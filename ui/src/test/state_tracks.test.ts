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
  await pth.openTraceFile('state_hierarchy.pb');
});

test('load trace', async () => {
  await pth.waitForIdleAndScreenshot('loaded.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

test('state details panel', async () => {
  const track = pth.locateTrack('ParentFolder/StateTrackA');
  await track.scrollIntoViewIfNeeded();

  const box = await track.boundingBox();
  if (box === null) {
    throw new Error('Track bounding box is null');
  }

  const shell = track.locator('.pf-track__shell');
  const shellBox = await shell.boundingBox();
  const sidebarWidth = shellBox ? shellBox.width : 150;

  // Click in the middle of the track's timeline canvas (which corresponds to 2s, the center of "Active" slice)
  const clickX = box.x + sidebarWidth + (box.width - sidebarWidth) / 2;
  const clickY = box.y + box.height / 2;

  await page.mouse.click(clickX, clickY);
  await pth.waitForPerfettoIdle();

  // Verify that the details panel opened and displays the correct state slice details
  const title = page.locator('.pf-details-shell h1.pf-header-title');
  const desc = page.locator('.pf-details-shell span.pf-header-description');

  await expect(title).toHaveText('Slice');
  await expect(desc).toHaveText('Active');

  // Take a screenshot of the details panel for visual verification
  await pth.waitForIdleAndScreenshot('state_details_panel.png', {
    locator: page.locator('.pf-drawer-panel'),
  });
});
