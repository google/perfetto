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

// ui_hierarchy.perfetto-trace has two synthetic Shell transitions spliced
// into a real trace: TO_FRONT (id 41) over snapshots 2-3 and
// KEYGUARD_OCCLUDE (id 42) over snapshots 4-5 of NotificationShade.

import {test, expect, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('ui_hierarchy.perfetto-trace');
});

test('transitions under the scrubber', async () => {
  await page.locator('.pf-sidebar').getByText('UI Hierarchy').click();
  await pth.waitForPerfettoIdle();
  const marks = page.locator('.pf-uih-scrub__transition');
  await expect(marks).toHaveCount(2);
  await expect(marks.first()).toHaveAttribute('title', /^Transition TO_FRONT/);
});

test('clicking a transition compares across it', async () => {
  await page.locator('.pf-uih-scrub__transition').first().click();
  await pth.waitForPerfettoIdle();
  // Shows the last snapshot in the transition against the one before it.
  await expect(page.locator('span.pf-uih-bar__pos')).toHaveText('3 / 6');
  await expect(page.locator('.pf-uih-diff-base')).toContainText(
    'vs 1 · before TO_FRONT',
  );
  await pth.waitForIdleAndScreenshot('compare.png');
});
