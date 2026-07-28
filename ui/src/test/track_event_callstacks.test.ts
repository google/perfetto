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

import {expect, test, type Locator, type Page} from '@playwright/test';
import {ensureExists} from '../base/assert';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

// Tests for the "Track Event Callstacks" area selection flamegraph. The trace
// contains six slices with inline callstacks; every begin event carries:
// - callstack_weight: bytes allocated (total 16128)
// - an "objects" debug annotation (total 60)
// - an "alloc_stats.latency_us" proto extension field (descriptor embedded)

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;
let drawerPanel: Locator;

const measurePicker = (name: string) =>
  page.locator('.pf-flamegraph .filter-bar button', {hasText: name}).first();

const menuItem = (text: string) =>
  page.locator('.pf-popup-content .pf-menu-item', {hasText: text}).first();

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('track_event_callstack_weights.pftrace');
  drawerPanel = page.locator('.pf-drawer-panel__drawer');
});

test('weight flamegraph', async () => {
  // Area-select the whole Allocations track. Anchor the drag just right of
  // the track shell so the selection starts before the first slice's ts.
  const track = pth.locateTrack('Global Track Events/Allocations');
  await track.scrollIntoViewIfNeeded();
  const box = ensureExists(await track.boundingBox());
  const shell = ensureExists(
    await track.locator('.pf-track__shell').boundingBox(),
  );
  const midY = box.y + box.height / 2;
  await page.mouse.move(shell.x + shell.width + 3, midY);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width - 10, midY, {steps: 10});
  await page.mouse.up();
  await pth.waitForPerfettoIdle();

  await page.locator('button[label="Track Event Callstacks"]').click();

  // The flamegraph must default to the Weight measure (weighted samples
  // exist) and aggregate all six samples: root shows 16,128.
  await pth.waitForIdleAndScreenshot('weight-flamegraph.png', {
    locator: drawerPanel,
  });
});

test('samples measure', async () => {
  await measurePicker('Weight').click();
  await pth.waitForPerfettoIdle();
  await menuItem('Samples').click();
  await pth.waitForIdleAndScreenshot('samples-flamegraph.png', {
    locator: drawerPanel,
  });
});

test('measure picker menu', async () => {
  await measurePicker('Samples').click();
  await pth.waitForPerfettoIdle();
  await menuItem('Add measure').click();
  await pth.waitForPerfettoIdle();

  // The searchable submenu must list both the debug annotation
  // (debug.objects) and the proto extension field (alloc_stats.latency_us).
  // Screenshot the whole page: popups render in a portal outside the drawer.
  await pth.waitForIdleAndScreenshot('measure-picker.png');
});

test('debug annotation measure', async () => {
  await menuItem('debug.objects').click();
  // The added measure becomes selected: root shows 60 objects.
  await pth.waitForIdleAndScreenshot('objects-flamegraph.png', {
    locator: drawerPanel,
  });
});

test('proto extension measure', async () => {
  await measurePicker('debug.objects').click();
  await pth.waitForPerfettoIdle();
  await menuItem('Add measure').click();
  await pth.waitForPerfettoIdle();
  await menuItem('alloc_stats.latency_us').click();
  // Total latency across all six samples: 75.8us.
  await pth.waitForIdleAndScreenshot('latency-flamegraph.png', {
    locator: drawerPanel,
  });
});

test('focus modes are filter chips', async () => {
  const filterInput = page.locator(
    '.pf-flamegraph .filter-bar > .pf-tag-input input',
  );
  const filterChips = page.locator(
    '.pf-flamegraph .filter-bar > .pf-tag-input .pf-chip',
  );

  await filterInput.fill('P: ParseInput');
  await filterInput.press('Enter');
  await pth.waitForPerfettoIdle();
  await expect(filterChips).toHaveCount(1);
  await expect(filterChips.first()).toContainText('Pivot: ParseInput');
  await pth.waitForIdleAndScreenshot('pivot-filter.png', {
    locator: drawerPanel,
  });

  // Show From Frame replaces Pivot rather than adding a second focus mode.
  await filterInput.fill('SFF: AllocateBuffer');
  await filterInput.press('Enter');
  await pth.waitForPerfettoIdle();
  await expect(filterChips).toHaveCount(1);
  await expect(filterChips.first()).toContainText(
    'Show From Frame: AllocateBuffer',
  );

  // Selecting a direction also clears the mutually exclusive focus mode.
  await page
    .locator('.pf-flamegraph .pf-radio-group__button', {hasText: 'Bottom Up'})
    .click();
  await pth.waitForPerfettoIdle();
  await expect(filterChips).toHaveCount(0);
});

test('highlight controls can be kept open or hidden', async () => {
  const highlightButton = page.locator(
    '.pf-flamegraph .filter-bar > .pf-button',
    {hasText: 'Highlight'},
  );

  await highlightButton.click();
  const highlightRow = page.locator(
    '.pf-flamegraph .pf-flamegraph-secondary-row',
  );
  await expect(highlightRow).toBeVisible();

  await highlightRow.locator('input').fill('ParseInput');
  await expect(
    highlightRow.locator('.pf-flamegraph-highlight-search__count'),
  ).toContainText('match');
  await pth.waitForIdleAndScreenshot('highlight-controls.png', {
    locator: drawerPanel,
  });

  // Hiding the controls preserves the active highlight.
  await highlightRow.locator('.pf-button', {hasText: 'Hide'}).click();
  await expect(highlightRow).toHaveCount(0);
  await expect(highlightButton).toHaveClass(/pf-active/);
});
