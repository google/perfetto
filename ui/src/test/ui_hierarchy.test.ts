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

// ui_hierarchy.perfetto-trace: six SystemUI snapshots (NotificationShade
// et al., 278 View/Compose nodes) of a real lockscreen -> shade trace.

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

// The layout, hierarchy and properties panes, without the snapshot bar
// (whose slider also shows the transitions, added by a later change).
function panes() {
  return {locator: page.locator('.pf-uih-main')};
}

async function canvasPixels(): Promise<string> {
  return page
    .locator('canvas.pf-uih-rects-canvas')
    .first()
    .evaluate((c) => (c as HTMLCanvasElement).toDataURL());
}

test('opens the NotificationShade hierarchy', async () => {
  await page.locator('.pf-sidebar').getByText('UI Hierarchy').click();
  await pth.waitForPerfettoIdle();
  // The default window is NotificationShade, at its first of 6 snapshots.
  await expect(page.locator('span.pf-uih-bar__pos')).toHaveText('1 / 6');
  expect(await page.locator('.pf-uih-tree__row').count()).toBeGreaterThan(50);
  await pth.waitForIdleAndScreenshot('viewer.png', panes());
});

test('search finds the Clear all button', async () => {
  const search = page.getByPlaceholder(
    'Search name, id, text, tag, description...',
  );
  await search.fill('Clear all');
  await pth.waitForPerfettoIdle();
  const row = page
    .locator('.pf-uih-tree__row')
    .filter({hasText: 'Clear all'})
    .first();
  await expect(row).toBeVisible();
  await row.click();
  await pth.waitForPerfettoIdle();
  // The properties of the selected node show its text and bounds.
  const props = page.locator('.pf-uih-props-section');
  await expect(page.locator('.pf-uih-props-header__name')).toHaveText(
    'dismiss_text',
  );
  const view = props.filter({hasText: 'Bounds'}).first();
  await expect(view).toContainText('[147, 339, 573, 423]');
  await expect(view).toContainText('Clear all notifications.');
  // This legacy trace predates stable node ids: id 207 is history_button
  // in the later snapshots, which the change history calls out.
  await expect(page.locator('.pf-uih-history__note')).toBeVisible();
  await expect(page.locator('.pf-uih-history__changes').nth(1)).toContainText(
    'different node: View "dismiss_text" → View "history_button"',
  );
  await pth.waitForIdleAndScreenshot('clear_all_selected.png', panes());
});

test('3D stack renders', async () => {
  await page
    .getByPlaceholder('Search name, id, text, tag, description...')
    .fill('');
  await pth.waitForPerfettoIdle();
  const flat = await canvasPixels();
  await page.locator('label').filter({hasText: '3D Stack'}).click();
  await pth.waitForPerfettoIdle();
  expect(await canvasPixels()).not.toEqual(flat);
  await pth.waitForIdleAndScreenshot('3d_stack.png', panes());
  await page.locator('label').filter({hasText: '3D Stack'}).click();
  await pth.waitForPerfettoIdle();
});
