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
import {spawn, type ChildProcess} from 'child_process';
import fs from 'fs';
import os from 'os';
import path from 'path';
import {getFreePort, waitForServer} from './perfetto_ui_test_helper';

test.skip(
  process.env.BIGTRACE_E2E !== '1' && process.env.BIGTRACE_E2E !== 'true',
  'Set BIGTRACE_E2E=1 to run Bigtrace live server E2E suite',
);

let page: Page;
let bigtraceServerProc: ChildProcess | undefined;
let bigtracePort: number;
let tracesDir: string;
let targetUrl: string;

async function killProcess(proc?: ChildProcess): Promise<void> {
  if (proc === undefined || proc.exitCode !== null) return;
  const exitPromise = new Promise((resolve) => proc.once('exit', resolve));
  proc.kill('SIGTERM');
  const timer = setTimeout(() => proc.kill('SIGKILL'), 3000);
  await exitPromise;
  clearTimeout(timer);
}

test.describe('Bigtrace UI E2E suite', () => {
  test.describe.configure({mode: 'serial'});

  test.beforeAll(async ({browser}) => {
    test.setTimeout(120_000);

    // 1. Create a clean temp directory with trace symlinks.
    tracesDir = fs.mkdtempSync(path.join(os.tmpdir(), 'bt_e2e_traces_'));
    const candidateDirs = [
      process.env.BIGTRACE_TRACES,
      path.resolve(__dirname, '../../../test/data'),
      path.resolve(__dirname, '../../../../dev/perfetto/test/data'),
      path.resolve(os.tmpdir(), 'bt_test_traces'),
    ].filter((d): d is string => Boolean(d && fs.existsSync(d)));

    for (const d of candidateDirs) {
      const traces = fs
        .readdirSync(d)
        .filter((f) => f.endsWith('.perfetto-trace'));
      if (traces.length > 0) {
        for (const f of traces) {
          try {
            fs.symlinkSync(path.join(d, f), path.join(tracesDir, f));
          } catch {
            // Ignore
          }
        }
        break;
      }
    }
    expect(
      fs.readdirSync(tracesDir).filter((f) => f.endsWith('.perfetto-trace'))
        .length,
    ).toBeGreaterThan(0);

    // 2. Start Bigtrace Python backend on a free port.
    bigtracePort = await getFreePort();
    const pythonBin = process.env.BIGTRACE_PYTHON || 'python3';
    const serverScript = path.resolve(
      __dirname,
      '../../../tools/bigtrace_server.py',
    );
    const shellPath =
      process.env.BIGTRACE_SHELL ||
      path.resolve(__dirname, '../../../out/release/trace_processor_shell');

    bigtraceServerProc = spawn(
      pythonBin,
      [
        serverScript,
        tracesDir,
        '-p',
        String(bigtracePort),
        '--shell-path',
        shellPath,
      ],
      {
        stdio: 'ignore',
      },
    );

    const backendReady = await waitForServer(
      `http://127.0.0.1:${bigtracePort}/v1/`,
      20_000,
    );
    expect(backendReady).toBe(true);

    targetUrl = `/bigtrace.html?server=http://127.0.0.1:${bigtracePort}/v1`;
    page = await browser.newPage();
  });

  test.afterAll(async () => {
    await killProcess(bigtraceServerProc);
    if (tracesDir !== '' && fs.existsSync(tracesDir)) {
      try {
        fs.rmSync(tracesDir, {recursive: true, force: true});
      } catch {
        // Ignore
      }
    }
    if (page !== undefined) {
      await page.close();
    }
  });

  test.beforeEach(async () => {
    await page.goto(targetUrl, {waitUntil: 'domcontentloaded'});
    await ensureEditorOpen();
  });

  async function ensureEditorOpen() {
    const editor = page.locator('.pf-editor .cm-content');
    const startQueryBtn = page.locator('button:has-text("Start Query")');
    const winner = await Promise.race([
      editor
        .waitFor({state: 'visible', timeout: 2000})
        .then(() => 'editor' as const)
        .catch(() => null),
      startQueryBtn
        .waitFor({state: 'visible', timeout: 2000})
        .then(() => 'btn' as const)
        .catch(() => null),
    ]);
    if (winner === 'btn') {
      await startQueryBtn.click();
    }
    await editor.waitFor({state: 'visible', timeout: 10_000});
  }

  async function setEditorText(sql: string) {
    const editor = page.locator('.pf-editor .cm-content');
    await editor.click();
    await page.keyboard.press('ControlOrMeta+A');
    await page.keyboard.press('Backspace');
    await page.keyboard.insertText(sql);
  }

  async function clickRun() {
    const runBtn = page
      .locator('.pf-bt-query-page__toolbar button:has-text("Run")')
      .first();
    await expect(runBtn).toBeEnabled();
    await runBtn.click();

    const overwriteBtn = page.locator('button:has-text("Overwrite")');
    const runningBar = page.locator('.pf-bt-status-bar--running');
    const outcome = await Promise.race([
      overwriteBtn
        .waitFor({state: 'visible', timeout: 1500})
        .then(() => 'modal' as const)
        .catch(() => null),
      runningBar
        .waitFor({state: 'visible', timeout: 1500})
        .then(() => 'running' as const)
        .catch(() => null),
    ]);
    if (outcome === 'modal') {
      await overwriteBtn.click();
    }
  }

  test('connect to server endpoint and open Bigtrace UI', async () => {
    test.setTimeout(60_000);
    const connBtn = page.locator('.pf-button-group button').first();
    await expect(connBtn).toBeVisible({timeout: 15_000});
    await expect(connBtn.locator('i')).toHaveText('cloud_done');
  });

  test('run a trace query and see rows in results grid', async () => {
    test.setTimeout(60_000);
    await setEditorText(
      'SELECT id, name, ts, dur, track_id FROM slice LIMIT 10',
    );
    await clickRun();

    const statusBar = page.locator('.pf-bt-status-bar');
    await expect(statusBar).toBeVisible({timeout: 20_000});
    await expect(page.locator('.pf-bt-status-bar--running')).toHaveCount(0, {
      timeout: 30_000,
    });

    const summary = page.locator('.pf-bt-results-summary');
    await expect(summary).toBeVisible({timeout: 10_000});
    await expect(summary).toContainText(/Showing \d+ rows|Returned \d+ rows/);
    await expect(
      page.locator('.pf-bt-query-page__results').first(),
    ).toBeVisible({timeout: 10_000});
    await expect(page.locator('.pf-bt-results-error')).toHaveCount(0);
  });

  test('limits including 0 (= no limit) preserved across rerun', async () => {
    test.setTimeout(60_000);
    const limitInput = page.locator('.pf-bt-limit-input input');
    await expect(limitInput).toBeVisible({timeout: 10_000});
    await expect(limitInput).toHaveAttribute(
      'title',
      'Maximum rows this query returns (0 = unlimited).',
    );

    await limitInput.click();
    await limitInput.fill('0');
    await clickRun();

    await expect(page.locator('.pf-bt-status-bar--running')).toHaveCount(0, {
      timeout: 30_000,
    });
    const statusVal = page.locator('.pf-bt-status-bar-stat-value').last();
    await expect(statusVal).toHaveAttribute('title', /no limit/);
    await expect(limitInput).toHaveValue('0');
    await expect(page.locator('.pf-bt-results-summary')).toContainText(
      /Showing \d+ rows/,
    );
    await expect(page.locator('.pf-bt-results-error')).toHaveCount(0);
  });

  test('multi-query (slices + sched) in one run', async () => {
    test.setTimeout(90_000);
    const addQueryBtn = page.locator('button:has-text("Add Query")').first();
    await expect(addQueryBtn).toBeVisible({timeout: 10_000});
    await addQueryBtn.click();

    const subqueryBar = page.locator('.pf-bt-subquery-bar');
    await expect(subqueryBar).toBeVisible({timeout: 10_000});
    const pills = subqueryBar.locator('.pf-bt-subquery-pill');
    await expect(pills).toHaveCount(2);

    // Query #1: already executed in previous test, so double-click rename is locked
    await pills.nth(0).locator('button').first().click();
    await pills.nth(0).locator('button').first().dblclick();
    await expect(
      pills.nth(0).locator('input.pf-bt-subquery-rename-input'),
    ).toHaveCount(0);
    await setEditorText(
      'SELECT id, name, ts, dur, track_id FROM slice LIMIT 10',
    );

    // Query #2: not executed yet, so double-click inline rename on the chip works
    await pills.nth(1).locator('button').first().click();
    await pills.nth(1).locator('button').first().dblclick();
    const renameInput1 = pills
      .nth(1)
      .locator('input.pf-bt-subquery-rename-input');
    await expect(renameInput1).toBeVisible({timeout: 5000});
    await renameInput1.fill('sched');
    await renameInput1.press('Enter');
    await expect(pills.nth(1)).toContainText('sched');
    await setEditorText('SELECT id, ts, dur, cpu, utid FROM sched LIMIT 10');

    // Click Run -> slot #0 already ran so Overwrite Table modal appears; click Overwrite
    await clickRun();
    const overwriteBtn = page.locator('.pf-modal button:has-text("Overwrite")');
    if (await overwriteBtn.isVisible({timeout: 3000}).catch(() => false)) {
      await overwriteBtn.click();
    }
    await expect(page.locator('.pf-bt-status-bar--running')).toHaveCount(0, {
      timeout: 40_000,
    });
    await expect(page.locator('.pf-bt-results-error')).toHaveCount(0);

    // After running, Query #2 is now locked against rename too
    await pills.nth(1).locator('button').first().dblclick();
    await expect(
      pills.nth(1).locator('input.pf-bt-subquery-rename-input'),
    ).toHaveCount(0);

    // Pill #1: verify row counts and track_id column header
    await pills.nth(0).locator('button').first().click();
    await expect(pills.nth(0)).toContainText(/\(\d+\)/);
    await expect(page.locator('.pf-bt-results-summary')).toContainText(
      /Showing \d+ rows/,
    );
    await expect(
      page.locator('.pf-bt-query-page__results').first(),
    ).toContainText('track_id');

    // Pill #2: verify row counts and cpu/utid column header
    await pills.nth(1).locator('button').first().click();
    await expect(pills.nth(1)).toContainText(/\(\d+\)/);
    await expect(page.locator('.pf-bt-results-summary')).toContainText(
      /Showing \d+ rows/,
    );
    await expect(
      page.locator('.pf-bt-query-page__results').first(),
    ).toContainText('cpu');
  });

  test('switch to DuckDB SQL mode and run |> pipeline and pf_interval_intersection over extracted tables', async () => {
    test.setTimeout(90_000);
    const modeBtn = page.locator('.pf-button-group button').nth(1);
    await expect(modeBtn).toBeVisible({timeout: 10_000});
    await modeBtn.click();
    await page
      .locator('.pf-popup-content button:has-text("DuckDB SQL")')
      .click();

    // Persistent switch, Rows limit input, and Settings button are hidden in DuckDB Mode
    await expect(page.locator('.pf-bt-limit-input')).toHaveCount(0);

    await setEditorText(
      `INTERVAL INTERSECTION OF (
        (FROM slice |> WHERE dur >= 0) AS s,
        (FROM sched |> WHERE dur >= 0) AS c
      ) PER (_trace_uuid)
      |> AGGREGATE COUNT(*) AS cnt`,
    );

    await clickRun();
    await expect(page.locator('.pf-bt-status-bar--running')).toHaveCount(0, {
      timeout: 40_000,
    });
    await expect(page.locator('.pf-bt-status-bar')).toBeVisible();
    await expect(page.locator('.pf-bt-results-error')).toHaveCount(0);
    await expect(
      page.locator('.pf-bt-query-page__results').first(),
    ).toBeVisible({timeout: 10_000});
  });

  test('cancel query in flight', async () => {
    test.setTimeout(60_000);
    // Switch to PerfettoSQL if tab was left in DuckDB mode
    const modeBtn = page.locator('.pf-button-group button').nth(1);
    if ((await modeBtn.textContent())?.includes('DuckDB')) {
      await modeBtn.click();
      await page
        .locator('.pf-popup-content button:has-text("PerfettoSQL")')
        .click();
      await ensureEditorOpen();
    }

    await setEditorText(
      'SELECT s1.id FROM slice s1 JOIN slice s2 ON s1.dur = s2.dur JOIN slice s3 ON s2.dur = s3.dur',
    );

    await clickRun();

    const cancelBtn = page.locator(
      '.pf-bt-query-page__toolbar button:has-text("Cancel")',
    );
    await expect(cancelBtn).toBeVisible({timeout: 5000});
    await cancelBtn.click();

    await expect(
      page.locator('.pf-bt-query-page__toolbar button:has-text("Run")').first(),
    ).toBeVisible({timeout: 10_000});
    await expect(page.locator('.pf-bt-status-bar--running')).toHaveCount(0);
  });

  test('history panel displays completed executions and supports staging DuckDB queries', async () => {
    test.setTimeout(60_000);
    const historyItems = page.locator('.pf-query-history__item');
    await expect(historyItems.first()).toBeVisible({timeout: 15_000});
    expect(await historyItems.count()).toBeGreaterThan(0);

    const duckdbIconBtn = page
      .locator(
        '.pf-query-history__item button:has-text("database"), .pf-query-history__item button[title*="DuckDB"]',
      )
      .first();
    await expect(duckdbIconBtn).toBeVisible({timeout: 5000});
    await duckdbIconBtn.click();
    const editor = page.locator('.pf-editor .cm-content').last();
    await expect(editor).toContainText('FROM');
  });
});
