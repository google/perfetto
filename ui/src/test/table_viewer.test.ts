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

import {test, Page, Locator} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'parallel'});

let pth: PerfettoTestHelper;
let page: Page;

async function clickTableHeader(headerName: string | RegExp) {
  await page
    .locator('.pf-content tr.header a.pf-anchor', {hasText: headerName})
    .click();
}

function getTableCells(text?: string | RegExp): Locator {
  return page.locator(
    '.pf-details-shell .generic-table tr:not(.header) a.pf-anchor',
    {
      hasText: text,
    },
  );
}

async function clickFirstTableCell(text: string | RegExp) {
  await getTableCells(text).nth(0).click();
}

test.beforeEach(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
});

test('slices with same name', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  const sliceName = 'LatencyInfo.Flow';
  await pth.searchSlice(sliceName);
  await page
    .locator('.pf-details-shell a.pf-anchor', {hasText: sliceName})
    .click();
  await pth.clickMenuItem('Slices with the same name');
  await clickTableHeader(/^id/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`slices-with-same-name.png`);
});

test('Table interactions', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.slice');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^id/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`slices-table.png`);

  // Hide `category` column.
  await clickTableHeader('category');
  await pth.clickMenuItem('Hide');
  await pth.waitForIdleAndScreenshot(`slices-table-hide-column.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  clickTableHeader(/^dur/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`slices-table-sorted.png`);

  // Filter out all "EventLatency" slices.
  clickFirstTableCell('EventLatency');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`slices-table-filter1.png`);

  // Filter to thread-only slices by clicking on the second NULL value.
  await getTableCells('NULL').nth(1).click();
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('is not null');
  await pth.waitForIdleAndScreenshot(`slices-table-filter2.png`);

  // Filter to LatencyInfo.Flow events.
  clickFirstTableCell('LatencyInfo.Flow');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem(/^equals/);
  await pth.waitForIdleAndScreenshot(`slices-table-filter3.png`);

  // Add argument.
  await clickTableHeader(/^name/);
  await pth.clickMenuItem('Add column');
  await pth.clickMenuItem('arg_set_id');
  await pth.clickMenuItem('chrome_latency_info.trace_id');
  await pth.waitForIdleAndScreenshot(`slices-table-add-argument.png`);

  // Sort by argument.
  await clickTableHeader('chrome_latency_info.trace_id');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`slices-table-sort-by-argument.png`);

  // Sort by argument.
  await clickFirstTableCell('3390');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`slices-table-filter-by-argument.png`);
});

//
// These tests check that the "go to" functionality for ID columns of the key tables
// (slice, sched, thread_state, process and thread) —- for example, clicking on a
// slice id column should update the selection and focus the relevant slice.
//

test('Go to slice', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.slice');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^id/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickTableHeader(/^dur/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first slice.
  await getTableCells().nth(0).click();
  await pth.waitForIdleAndScreenshot(`go-to.png`);

  // Go to current selection tab.
  await pth.switchToTab('Current Selection');
  await pth.waitForIdleAndScreenshot(`current-selection.png`);
});

test('Go to thread_state', async () => {
  // Open Android trace with kernel scheduling data.
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.thread_state');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^id/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickTableHeader(/^dur/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Filter out sleeps.
  await clickFirstTableCell(/^S/);
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`filtered.png`);

  // Go to the first thread_state.
  await getTableCells().nth(0).click();
  await pth.waitForIdleAndScreenshot(`go-to.png`);

  // Go to current selection tab.
  await pth.switchToTab('Current Selection');
  await pth.waitForIdleAndScreenshot(`current-selection.png`);
});

test('Go to sched', async () => {
  // Open Android trace with kernel scheduling data.
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.sched');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^id/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickTableHeader(/^dur/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Filter out idle.
  await clickFirstTableCell('swapper');
  await pth.clickMenuItem('Add filter');
  await getTableCells('120').nth(0).click();
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`filtered.png`);

  // Go to the first slice.
  await getTableCells().nth(0).click();
  await pth.waitForIdleAndScreenshot(`go-to.png`);

  // Go to current selection tab.
  await pth.switchToTab('Current Selection');
  await pth.waitForIdleAndScreenshot(`current-selection.png`);
});

//
// For process and thread tables, we open a new tab instead of updating selection.
//

test('Go to process', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.process');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^upid/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickTableHeader(/^name/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first process.
  await getTableCells().nth(0).click();
  await pth.clickMenuItem('Show process details');
  await pth.waitForIdleAndScreenshot(`go-to.png`);
});

test('Go to thread', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('perfetto.ShowTable.thread');
  // Sort the table by id for consistent ordering.
  await clickTableHeader(/^utid/);
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickTableHeader(/^name/);
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first thread.
  await getTableCells().nth(0).click();
  await pth.clickMenuItem('Show thread details');
  await pth.waitForIdleAndScreenshot(`go-to.png`);
});
