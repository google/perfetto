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

import {expect, type Page, test} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

// Structural (screenshot-free) parity coverage of the heap-profile
// area-selection collection: the plugin auto-selects the heap profile area
// on trace load, so the tab is reachable without manual area dragging. The
// trace contains one profiled process; multi-process behaviour is covered by
// the SQL-level unit tests (heap_profile_metrics_unittest.ts).

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('system-server-native-profile');
});

test('heap profile selection opens as a process collection', async () => {
  // This trace's heapprofd heap is named "unknown", so it surfaces as a
  // generic heap profile.
  await page
    .locator('button', {hasText: 'Profile: unknown flamegraph'})
    .first()
    .click();
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection')).toBeVisible();
  await expect(page.locator('.pf-flamegraph-collection__summary')).toHaveText(
    'Merging 1 of 1 processes',
  );
  await expect(page.locator('.pf-flamegraph')).toBeVisible();
});

test('grid lists the profiled process with its totals', async () => {
  const grid = page.locator('.pf-flamegraph-collection__grid');
  await expect(grid).toBeVisible();
  await expect(grid).toContainText('process');
  await expect(grid).toContainText('Total Size');
  await expect(grid).toContainText('Process 11686');
});

test('the shown process is highlighted in the grid', async () => {
  // The entry key (the upid) differs from the displayed process label, so
  // the highlight must key off the row, not the label.
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
  const current = page.locator('.pf-flamegraph-collection__entry--current');
  await expect(current).toHaveCount(1);
  await expect(current).toHaveText('Process 11686');
  // The step title shows the process label, not the raw upid key.
  await expect(
    page.locator('.pf-flamegraph-collection__flame-cell-title'),
  ).toHaveText('Process 11686');

  // Merge back on for the tests that follow.
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
});

test('unreleased and total measures are selectable', async () => {
  // The metric selector lives in the flamegraph filter bar.
  await page.locator('.pf-flamegraph .filter-bar button').first().click();
  const menu = page.locator('.pf-popup-content');
  await expect(menu).toContainText('Unreleased Size');
  await expect(menu).toContainText('Total Size');
  await page.keyboard.press('Escape');
});
