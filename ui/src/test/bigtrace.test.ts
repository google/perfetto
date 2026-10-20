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

// Drives the Bigtrace UI (served on :10000, e.g. by
// `ui/run-dev-server --bigtrace`) against a local tools/bigtrace_server.py:
//   cd ui && BIGTRACE_E2E=1 [BIGTRACE_SHELL=<trace_processor_shell>] \
//     [BIGTRACE_TRACES=<dir of *.perfetto-trace>] \
//     node_modules/.bin/playwright test src/test/bigtrace.test.ts

import {
  test,
  type Locator,
  type Page,
  expect as baseExpect,
} from '@playwright/test';
import {spawn, type ChildProcess} from 'child_process';
import fs from 'fs';
import net from 'net';
import os from 'os';
import path from 'path';

test.skip(
  !['1', 'true'].includes(process.env.BIGTRACE_E2E ?? ''),
  'Set BIGTRACE_E2E=1 to run the Bigtrace live server E2E suite',
);

// Queries take a while to fan out over the traces.
const expect = baseExpect.configure({timeout: 30_000});
const ROOT = path.resolve(__dirname, '../../..');
// Table names unique to this run, in case the server keeps state across runs.
const RUN = Date.now().toString(36);
const SLICES_TABLE = `e2e_slices_${RUN}`;
const SCHED_TABLE = `e2e_sched_${RUN}`;
const JOINED_TABLE = `e2e_joined_${RUN}`;
const SLICES = 'SELECT id, name, ts, dur, track_id FROM slice LIMIT 10';
const SCHED = 'SELECT id, ts, dur, cpu, utid FROM sched LIMIT 10';

let page: Page;
let server: ChildProcess | undefined;
let tracesDir = '';
let url = '';

function freePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = net.createServer().listen(0, '127.0.0.1', () => {
      const {port} = srv.address() as net.AddressInfo;
      srv.close(() => resolve(port));
    });
    srv.on('error', reject);
  });
}

test.describe('Bigtrace', () => {
  test.describe.configure({mode: 'serial'});

  test.beforeAll(async ({browser}) => {
    test.setTimeout(120_000);
    const srcDir = process.env.BIGTRACE_TRACES ?? path.join(ROOT, 'test/data');
    tracesDir = fs.mkdtempSync(path.join(os.tmpdir(), 'bt_e2e_traces_'));
    for (const f of fs.readdirSync(srcDir)) {
      if (f.endsWith('.perfetto-trace')) {
        fs.symlinkSync(path.join(srcDir, f), path.join(tracesDir, f));
      }
    }

    const port = await freePort();
    let stderr = '';
    server = spawn(
      process.env.BIGTRACE_PYTHON ?? 'python3',
      [
        path.join(ROOT, 'tools/bigtrace_server.py'),
        tracesDir,
        '-p',
        String(port),
        '--shell-path',
        process.env.BIGTRACE_SHELL ??
          path.join(ROOT, 'out/release/trace_processor_shell'),
      ],
      {stdio: ['ignore', 'ignore', 'pipe']},
    );
    server.stderr?.on('data', (d: Buffer) => {
      stderr = (stderr + d.toString()).slice(-4000);
    });
    const endpoint = `http://127.0.0.1:${port}/v1`;
    await expect
      .poll(
        () =>
          fetch(`${endpoint}/`)
            .then((r) => r.ok)
            .catch(() => false),
        {timeout: 30_000, message: `server did not start:\n${stderr}`},
      )
      .toBe(true);

    url = `/bigtrace.html?server=${endpoint}`;
    page = await browser.newPage();
  });

  test.afterAll(async () => {
    server?.kill();
    fs.rmSync(tracesDir, {recursive: true, force: true});
    await page?.close();
  });

  // Every test starts from one fresh tab; the server's history carries over.
  test.beforeEach(async () => {
    await page.goto(url);
    await page.evaluate(() => localStorage.clear());
    await page.goto(url);
  });

  // The active editor tab's content; inactive tabs stay in the DOM, hidden.
  const view = () => page.locator('.pf-bt-editor-tab:visible');
  const editorTab = () =>
    page
      .locator('.pf-bt-query-page__editor-tabs .pf-tab-strip__tab--active')
      .first();
  const queryTabs = () =>
    view().locator('.pf-bt-query-page__editor-panel .pf-tab-strip__tab');
  const runButton = () =>
    view().locator('.pf-bt-query-page__toolbar button:has-text("Run")');
  const engineToggle = () =>
    page.locator('button[title^="Switch between PerfettoSQL"]');
  const summary = () => view().locator('.pf-bt-results-summary');
  const persistentSwitch = () =>
    view().locator(
      '.pf-bt-query-page__toolbar label:has-text("Persistent") input',
    );

  async function startQuery() {
    await page.locator('button:has-text("Start query")').click();
  }

  async function setSql(sql: string) {
    await view().locator('.pf-editor .cm-content').click();
    await page.keyboard.press('ControlOrMeta+A');
    await page.keyboard.insertText(sql);
  }

  async function rename(tab: Locator, name: string) {
    await tab.dblclick();
    await tab.locator('input').fill(name);
    await tab.locator('input').press('Enter');
  }

  test('connects to the local server, which offers DuckDB', async () => {
    await expect(
      page.locator('.pf-topbar i:text-is("cloud_done")'),
    ).toBeVisible();
    await expect(engineToggle()).toHaveText(/PerfettoSQL/);
  });

  test('a persistent query is named, run, and guarded against overwrite', async () => {
    await startQuery();
    await rename(editorTab(), SLICES_TABLE);
    await setSql(SLICES);
    await runButton().click();
    await expect(summary()).toContainText(/Showing \d+ rows/);
    await expect(editorTab().locator('.pf-tab-strip__tab-title')).toHaveText(
      SLICES_TABLE,
    );

    // Running again would overwrite the table: run a copy under the
    // suggested name in a new tab instead.
    await runButton().click();
    const modal = page.locator('.pf-modal-dialog');
    await expect(modal).toContainText(`"${SLICES_TABLE}" already exists`);
    await modal.locator('button:has-text("Create New Tab")').click();
    await expect(editorTab().locator('.pf-tab-strip__tab-title')).toHaveText(
      new RegExp(`^${SLICES_TABLE}_.+`),
    );
    await expect(summary()).toContainText(/Showing \d+ rows/);
  });

  test('several queries run in one pass, each into its own table', async () => {
    await startQuery();
    await setSql(SLICES);
    await page.locator('button:has-text("Add Query")').click();
    await expect(queryTabs()).toHaveCount(2);
    await rename(queryTabs().nth(1), SCHED_TABLE);
    await setSql(SCHED);

    await expect(runButton()).toHaveText(/Run 2 Queries/);
    await runButton().click();

    await queryTabs().nth(0).click();
    await expect(summary()).toContainText(/Showing \d+ rows/);
    await expect(
      view().locator('.pf-bt-query-page__results').first(),
    ).toContainText('track_id');
    await queryTabs().nth(1).click();
    await expect(
      view().locator('.pf-bt-query-page__results').first(),
    ).toContainText('utid');
    // A persisted table keeps its name.
    await queryTabs().nth(1).dblclick();
    await expect(queryTabs().nth(1).locator('input')).toHaveCount(0);
  });

  test('a running query can be cancelled, and errors are shown', async () => {
    await startQuery();
    // Default table names come from Math.random, which the Playwright config
    // seeds: an earlier test's unnamed query already took this tab's name.
    await rename(editorTab(), `e2e_cancel_${RUN}`);
    await setSql(
      'SELECT s1.id FROM slice s1 JOIN slice s2 ON s1.dur = s2.dur ' +
        'JOIN slice s3 ON s2.dur = s3.dur',
    );
    await runButton().click();
    await view()
      .locator('.pf-bt-query-page__toolbar button:has-text("Cancel")')
      .click();
    await expect(runButton()).toBeVisible();
    await expect(view().locator('.pf-bt-status-bar--running')).toHaveCount(0);

    await persistentSwitch().uncheck({force: true});
    await setSql('SELECT * FROM no_such_table');
    await runButton().click();
    await expect(view().locator('.pf-bt-error-content')).toContainText(
      'no_such_table',
    );
  });

  test('DuckDB SQL runs ephemeral by default and can persist a table', async () => {
    await engineToggle().click();
    await expect(engineToggle()).toHaveText(/DuckDB SQL/);
    await expect(view().locator('.pf-bt-limit-input')).toHaveCount(0);
    await expect(persistentSwitch()).not.toBeChecked();

    await setSql(
      `SELECT _trace_uuid, count(*) AS cnt FROM ${SLICES_TABLE} ` +
        `JOIN ${SCHED_TABLE} USING (_trace_uuid) GROUP BY _trace_uuid`,
    );
    await runButton().click();
    await expect(summary()).toContainText(/Returned \d+ rows/);

    await rename(editorTab(), JOINED_TABLE);
    await persistentSwitch().check({force: true});
    await runButton().click();
    await expect(summary()).toContainText(/Showing \d+ rows/);

    // A new tab stays in DuckDB mode and sees the persisted table.
    await page.locator('.pf-tabs__new-tab-btn').click();
    await expect(editorTab()).not.toHaveText(new RegExp(JOINED_TABLE));
    await setSql(`SELECT sum(cnt) AS total FROM ${JOINED_TABLE}`);
    await runButton().click();
    await expect(summary()).toContainText(/Returned 1 rows/);
  });

  test('history entries open, stage DuckDB queries, and delete', async () => {
    const item = page.locator('.pf-query-history__item').filter({
      has: page.locator('.pf-bt-history-item-table-link', {
        hasText: SCHED_TABLE,
      }),
    });
    await expect(item).toHaveCount(1);

    await item.locator('button[title="Open"]').click();
    await expect(editorTab().locator('.pf-tab-strip__tab-title')).toHaveText(
      SCHED_TABLE,
    );
    await expect(summary()).toContainText(/Showing \d+ rows/);

    await item.locator('button[title*="in DuckDB"]').click();
    await expect(engineToggle()).toHaveText(/DuckDB SQL/);
    await expect(view().locator('.pf-editor .cm-content')).toContainText(
      `FROM ${SCHED_TABLE}`,
    );

    await item.locator('button[title="Delete query"]').click();
    await page.locator('.pf-modal-dialog button:has-text("Delete")').click();
    await expect(item).toHaveCount(0);
  });
});
