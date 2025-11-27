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

import {test, Page, expect} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
});

test('omnibox query', async () => {
  const omnibox = page.locator('input[ref=omnibox]');
  await omnibox.focus();
  await omnibox.fill('foo');
  await omnibox.selectText();
  await omnibox.press(':');
  await pth.waitForPerfettoIdle();
  await omnibox.fill(
    'select id, ts, dur, name, track_id from slices limit 100',
  );
  await pth.waitForPerfettoIdle();
  await omnibox.press('Enter');

  await pth.waitForIdleAndScreenshot('query mode.png', {
    mask: [page.locator('.pf-query-table .pf-header-bar')],
  });

  page.locator('.pf-data-grid').getByText('17806091326279').click();
  await pth.waitForIdleAndScreenshot('row 1 clicked.png', {
    mask: [page.locator('.pf-query-table .pf-header-bar')],
  });

  page.locator('.pf-data-grid').getByText('17806092405136').click();
  await pth.waitForIdleAndScreenshot('row 2 clicked.png', {
    mask: [page.locator('.pf-query-table .pf-header-bar')],
  });

  // Clear the omnibox
  await omnibox.selectText();
  for (let i = 0; i < 2; i++) {
    await omnibox.press('Backspace');
    await pth.waitForPerfettoIdle();
  }
  await pth.waitForIdleAndScreenshot('omnibox cleared.png', {
    clip: {x: 0, y: 0, width: 1920, height: 100},
  });
});

test('query page', async () => {
  await pth.navigate('#!/query');
  await pth.waitForPerfettoIdle();
  const textbox = page.locator('.pf-editor div[role=textbox]');
  for (let i = 1; i <= 3; i++) {
    await textbox.focus();
    await textbox.clear();
    await textbox.fill(`select id, ts, dur, name from slices limit ${i}`);
    await textbox.press('ControlOrMeta+Enter');
    await textbox.blur();
    await pth.waitForIdleAndScreenshot(`query limit ${i}.png`, {
      mask: [page.locator('.pf-data-grid__toolbar')],
    });
  }

  // Now test the query history.
  page.locator('.pf-query-history .pf-query-history__item').nth(0).click();
  await pth.waitForPerfettoIdle();
  expect(await textbox.textContent()).toEqual(
    'select id, ts, dur, name from slices limit 3',
  );

  page.locator('.pf-query-history .pf-query-history__item').nth(2).click();
  await pth.waitForPerfettoIdle();
  expect(await textbox.textContent()).toEqual(
    'select id, ts, dur, name from slices limit 1',
  );

  // Double click on the 2nd one and expect the query is re-ran.
  page
    .locator('.pf-query-page .pf-query-history .pf-query-history__item')
    .nth(1)
    .dblclick();
  await pth.waitForPerfettoIdle();

  // Get all rows, but filter for *only* those that have a 'cell' inside to
  // avoid counting the header row.
  const dataRows = page.getByRole('row').filter({
    has: page.getByRole('cell'),
  });
  expect(await dataRows.count()).toEqual(2);
});
