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

// Locate the header cell with the given column name.
function locateHeaderCells(text?: string): Locator {
  const columnHeaders = page.getByRole('columnheader');
  if (text === undefined) {
    return columnHeaders;
  } else {
    return columnHeaders.filter({has: page.getByText(text, {exact: true})});
  }
}

// Locate the data cells, optionally filtered by text.
function locateDataCells(text?: string): Locator {
  const cells = page.getByRole('cell');
  if (text === undefined) {
    return cells;
  } else {
    return cells.filter({has: page.getByText(text, {exact: true})});
  }
}

async function clickColumnContextMenu(headerName?: string) {
  const cell = locateHeaderCells(headerName);
  await cell.hover(); // Hover to reveal the menu button.
  await cell.getByRole('button', {name: 'Column menu'}).click();
}

async function clickCellContextMenu(text?: string) {
  const cell = locateDataCells(text).nth(0);
  await cell.hover(); // Hover to reveal the menu button.
  await cell.getByRole('button', {name: 'Cell menu'}).click();
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
  await clickColumnContextMenu('id');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`slices-with-same-name.png`);
});

test('Table interactions', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('org.chromium.ShowTable.slice');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('id');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`slices-table.png`);

  // Hide `category` column.
  await clickColumnContextMenu('category');
  await pth.clickMenuItem('Hide');
  await pth.waitForIdleAndScreenshot(`slices-table-hide-column.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('dur');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`slices-table-sorted.png`);

  // Filter out all "EventLatency" slices.
  await clickCellContextMenu('EventLatency');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`slices-table-filter1.png`);

  // Filter to thread-only slices by clicking on the second NULL value.
  await clickCellContextMenu('null');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('is not null');
  await pth.waitForIdleAndScreenshot(`slices-table-filter2.png`);

  // Filter to LatencyInfo.Flow events.
  await clickCellContextMenu('LatencyInfo.Flow');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem(/^equals/);
  await pth.waitForIdleAndScreenshot(`slices-table-filter3.png`);

  // Add argument.
  await clickColumnContextMenu('name');
  await pth.clickMenuItem('Add column');
  await pth.clickMenuItem('arg_set_id');
  await pth.clickMenuItem('chrome_latency_info.trace_id');
  await pth.waitForIdleAndScreenshot(`slices-table-add-argument.png`);

  // Sort by argument.
  await clickColumnContextMenu('arg_set_id[chrome_latency_info.trace_id]');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`slices-table-sort-by-argument.png`);

  // Sort by argument.
  await clickCellContextMenu('3390');
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
  await pth.runCommand('org.chromium.ShowTable.slice');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('id');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('dur');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first slice.
  await locateDataCells().nth(0).locator('.pf-anchor').click();
  await pth.waitForIdleAndScreenshot(`go-to.png`);

  // Go to current selection tab.
  await pth.switchToTab('Current Selection');
  await pth.waitForIdleAndScreenshot(`current-selection.png`);
});

test('Go to thread_state', async () => {
  // Open Android trace with kernel scheduling data.
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  // Show the slice table via command.
  await pth.runCommand('org.chromium.ShowTable.thread_state');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('id');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('dur');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Filter out sleeps.
  await clickCellContextMenu('S');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals');
  await pth.waitForIdleAndScreenshot(`filtered.png`);

  // Go to the first thread_state.
  await locateDataCells().nth(0).locator('.pf-anchor').click();
  await pth.waitForIdleAndScreenshot(`go-to.png`);

  // Go to current selection tab.
  await pth.switchToTab('Current Selection');
  await pth.waitForIdleAndScreenshot(`current-selection.png`);
});

test('Go to sched', async () => {
  // This test can take a little longer
  test.setTimeout(60_000);

  // Open Android trace with kernel scheduling data.
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  // Show the slice table via command.
  await pth.runCommand('org.chromium.ShowTable.sched');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('id');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('dur');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Filter out idle.
  await clickCellContextMenu('120');
  await pth.clickMenuItem('Add filter');
  await pth.clickMenuItem('not equals to');
  await pth.waitForIdleAndScreenshot(`filtered.png`);

  // Go to the first slice.
  await locateDataCells().nth(0).click();
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
  await pth.runCommand('org.chromium.ShowTable.process');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('upid');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('name');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first process.
  await clickCellContextMenu();
  await pth.clickMenuItem('Show process details');
  await pth.waitForIdleAndScreenshot(`go-to.png`);
});

test('Go to thread', async () => {
  await pth.openTraceFile('chrome_scroll_without_vsync.pftrace');

  // Show the slice table via command.
  await pth.runCommand('org.chromium.ShowTable.thread');
  // Sort the table by id for consistent ordering.
  await clickColumnContextMenu('utid');
  await pth.clickMenuItem('Sort: lowest first');
  await pth.waitForIdleAndScreenshot(`open-table.png`);

  // Sort the table by dur in descending order. Note that we must explicitly exclude
  // the "thread_dur" column, as it also contains "dur" in its name.
  await clickColumnContextMenu('name');
  await pth.clickMenuItem('Sort: highest first');
  await pth.waitForIdleAndScreenshot(`sorted.png`);

  // Go to the first thread.
  await clickCellContextMenu();
  await pth.clickMenuItem('Show thread details');
  await pth.waitForIdleAndScreenshot(`go-to.png`);
});
