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

import {test, type Page, expect} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

const PLUGIN = 'dev.perfetto.SqlEditorIntelligence';

async function setQuery(page: Page, pth: PerfettoTestHelper, text: string) {
  const editor = page.locator('.cm-content');
  await editor.click();
  await page.keyboard.press('ControlOrMeta+a');
  await page.keyboard.press('Backspace');
  await page.keyboard.type(text);
  await pth.waitForPerfettoIdle();
}

// ---- Plugin ENABLED: completion + diagnostics work ----
test.describe('SqlEditorIntelligence enabled', () => {
  test.describe.configure({mode: 'serial'});
  let pth: PerfettoTestHelper;
  let page: Page;

  test.beforeAll(async ({browser}) => {
    page = await browser.newPage();
    pth = new PerfettoTestHelper(page);
    await pth.openTraceFile('api34_startup_cold.perfetto-trace', {
      enablePlugins: PLUGIN,
    });
    // Switch to the query page via the hash (no reload, so the trace + the
    // ?enablePlugins choice survive).
    await page.evaluate(() => (window.location.hash = '#!/query'));
    await pth.waitForPerfettoIdle();
    await page.locator('.cm-content').waitFor();
  });

  test('diagnostics underline an unknown table', async () => {
    await setQuery(page, pth, 'select foo from no_such_table_xyz');
    await expect(page.locator('.pf-cm-diag').first()).toBeVisible({
      timeout: 15000,
    });
  });

  test('a valid stdlib query has no diagnostics (no false positives)', async () => {
    await setQuery(page, pth, 'select id, ts, dur from slice limit 5');
    await page.waitForTimeout(2000);
    await expect(page.locator('.pf-cm-diag')).toHaveCount(0);
  });

  test('autocomplete offers stdlib tables after FROM', async () => {
    await setQuery(page, pth, 'select * from sl');
    await page.keyboard.press('Control+Space');
    await expect(page.locator('.cm-tooltip-autocomplete')).toBeVisible({
      timeout: 5000,
    });
  });
});

// ---- Plugin DISABLED (default): the editor is untouched ----
test.describe('SqlEditorIntelligence disabled', () => {
  test.describe.configure({mode: 'serial'});
  let pth: PerfettoTestHelper;
  let page: Page;

  test.beforeAll(async ({browser}) => {
    page = await browser.newPage();
    pth = new PerfettoTestHelper(page);
    // No enablePlugins → the plugin never activates.
    await pth.openTraceFile('api34_startup_cold.perfetto-trace');
    // Switch to the query page via the hash (no reload, so the trace + the
    // ?enablePlugins choice survive).
    await page.evaluate(() => (window.location.hash = '#!/query'));
    await pth.waitForPerfettoIdle();
    await page.locator('.cm-content').waitFor();
  });

  test('no diagnostics and no autocomplete when the plugin is off', async () => {
    await setQuery(page, pth, 'select foo from no_such_table_xyz');
    await page.waitForTimeout(3000);
    await expect(page.locator('.pf-cm-diag')).toHaveCount(0);

    await setQuery(page, pth, 'select * from sl');
    await page.keyboard.press('Control+Space');
    await page.waitForTimeout(1500);
    await expect(page.locator('.cm-tooltip-autocomplete')).toHaveCount(0);
  });
});
