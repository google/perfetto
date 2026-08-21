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

// Structural (screenshot-free) coverage of the stack-sample area-selection
// collection: the perf plugin auto-selects all samples on trace load, so the
// tab is reachable without manual area dragging.

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('perf_sample.pb');
});

test('perf samples open as a context collection', async () => {
  // The plugin auto-selects the whole perf sample area on trace ready, which
  // surfaces the flamegraph tab in the selection drawer.
  await page
    .locator('button', {hasText: 'Perf Sample Flamegraph'})
    .first()
    .click();
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection')).toBeVisible();
  await expect(
    page.locator('.pf-flamegraph-collection__summary'),
  ).toContainText(/Merging \d+ of \d+ contexts/);
  await expect(page.locator('.pf-flamegraph')).toBeVisible();
});

test('context grid lists per-thread rows with sample counts', async () => {
  const grid = page.locator('.pf-flamegraph-collection__grid');
  await expect(grid).toBeVisible();
  await expect(grid).toContainText('context');
  await expect(grid).toContainText('Samples');
});

test('merge off steps through the contexts', async () => {
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
  await expect(
    page.locator('.pf-flamegraph-collection__summary'),
  ).toContainText(/Showing \d+ of \d+ contexts/);
  await expect(
    page.locator('.pf-flamegraph-collection__flame-pos'),
  ).toContainText(/1 \/ \d+/);
  await expect(
    page.locator('.pf-flamegraph-collection__flame-cell-title'),
  ).not.toBeEmpty();

  // Merge back on for a stable end state.
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
  await expect(
    page.locator('.pf-flamegraph-collection__summary'),
  ).toContainText(/Merging \d+ of \d+ contexts/);
});

test('clicking a context name jumps to it and highlights it', async () => {
  // perf_sample.pb has a single sampled thread; open a trace with two
  // sampled processes to exercise jumping between contexts. The entry key
  // here (a context key like "upid=12") differs from the displayed label
  // (the process name), so the highlight and jump must key off the row,
  // not the label.
  await pth.openTraceFile('callstack_sampling.pftrace');
  await pth.runCommand('dev.perfetto.SelectAllPerfSamples');
  await pth.waitForPerfettoIdle();
  await page
    .locator('button', {hasText: 'Perf Sample Flamegraph'})
    .first()
    .click();
  await pth.waitForPerfettoIdle();
  const entries = page.locator(
    '.pf-flamegraph-collection__grid .pf-flamegraph-collection__entry',
  );
  const count = await entries.count();
  expect(count).toBeGreaterThan(1);
  const last = entries.nth(count - 1);
  const label = (await last.textContent()) ?? '';
  await last.click();
  await pth.waitForPerfettoIdle();
  await expect(
    page.locator('.pf-flamegraph-collection__summary'),
  ).toContainText(/Showing \d+ of \d+ contexts/);
  await expect(
    page.locator('.pf-flamegraph-collection__flame-pos'),
  ).toContainText(`${count} / ${count}`);
  const current = page.locator('.pf-flamegraph-collection__entry--current');
  await expect(current).toHaveCount(1);
  await expect(current).toHaveText(label);
  await expect(
    page.locator('.pf-flamegraph-collection__flame-cell-title'),
  ).toContainText(label);
});
