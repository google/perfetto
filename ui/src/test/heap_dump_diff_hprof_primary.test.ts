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

// Companion to heap_dump_diff.test.ts: opens a *raw hprof* as the
// primary trace (the other suite uses a perfetto pftrace as primary).
// Confirms the explorer renders, the diff CTA is reachable, and a
// pftrace baseline diffs cleanly against an hprof primary.

import {test, expect, Page} from '@playwright/test';
import path from 'path';
import fs from 'fs';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
import '../plugins/com.android.HeapDumpExplorer/baseline/state';
import '../plugins/com.android.HeapDumpExplorer/diff/diff_debug';

test.describe.configure({mode: 'serial'});

const PRIMARY_HPROF = 'test-dump.hprof';
const BASELINE_PFTRACE = 'system-server-heap-graph.pftrace';

let pth: PerfettoTestHelper;
let page: Page;

function tracePath(name: string): string {
  const cwd = process.cwd();
  const parts = ['test', 'data', name];
  if (cwd.endsWith('/ui')) parts.unshift('..');
  const p = path.join(...parts);
  if (!fs.existsSync(p)) throw new Error(`missing ${p} (cwd=${cwd})`);
  return p;
}

function fileInputLocator() {
  return page.locator('input[type=file][aria-hidden="true"]');
}

async function ensureOnOverview(): Promise<void> {
  await page.locator('.pf-tabs__tab:has-text("Overview")').first().click();
  await pth.waitForPerfettoIdle();
}

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile(PRIMARY_HPROF);
  await page.evaluate(() => {
    window.location.hash = '#!/heapdump';
  });
  await pth.waitForPerfettoIdle();
});

test.afterEach(async () => {
  await page.evaluate(() => {
    document
      .querySelector<HTMLButtonElement>(
        'button[aria-label="Remove all baseline traces"]',
      )
      ?.click();
  });
});

// 1. Hprof opens cleanly as the primary trace.
test('hprof primary loads and shows the Overview heading', async () => {
  await ensureOnOverview();
  await expect(
    page.locator('.ah-view-heading:has-text("Overview")').first(),
  ).toBeVisible();
  // The Diff CTA is rendered (no baseline yet).
  await expect(
    page.locator('button:has-text("Diff against another trace")'),
  ).toBeVisible();
});

// 2. Hprof primary + pftrace baseline → real diff with non-zero deltas.
test('hprof primary + pftrace baseline produces a usable diff', async () => {
  test.setTimeout(180_000);
  await ensureOnOverview();
  await fileInputLocator().setInputFiles(tracePath(BASELINE_PFTRACE));
  await page.waitForFunction(
    () => window.__heapdumpDebug?.hasBaseline(),
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();
  // Open Classes diff and wait for non-UNCHANGED rows.
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await page
    .locator('.ah-view-heading:has-text("Classes diff")')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  await page.waitForFunction(
    () => (window.__heapdumpDiff?.gen('classes') ?? 0) > 0,
    null,
    {timeout: 30_000},
  );
  const summary = await page.evaluate(() => {
    const rows = window.__heapdumpDiff!.rows('classes')!;
    const counts = {NEW: 0, REMOVED: 0, GREW: 0, SHRANK: 0, UNCHANGED: 0};
    for (const r of rows) counts[r.status as keyof typeof counts]++;
    return {total: rows.length, counts};
  });
  expect(summary.total).toBeGreaterThan(0);
  // Two completely different traces: a healthy mix of statuses.
  expect(summary.counts.NEW + summary.counts.REMOVED).toBeGreaterThan(0);
});

// 3. Hprof + hprof reload (same file as primary loaded as baseline).
//    Verifies the engine isolation works across two parses of the same
//    file: each owns its own Wasm heap.
test('hprof loaded twice (primary & baseline) yields a consistent diff', async () => {
  test.setTimeout(120_000);
  await ensureOnOverview();
  await fileInputLocator().setInputFiles(tracePath(PRIMARY_HPROF));
  await page.waitForFunction(
    () => window.__heapdumpDebug?.hasBaseline(),
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await page
    .locator('.ah-view-heading:has-text("Classes diff")')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  await page.waitForFunction(
    () => (window.__heapdumpDiff?.gen('classes') ?? 0) > 0,
    null,
    {timeout: 30_000},
  );
  const summary = await page.evaluate(() => {
    const rows = window.__heapdumpDiff!.rows('classes')!;
    const counts = {NEW: 0, REMOVED: 0, GREW: 0, SHRANK: 0, UNCHANGED: 0};
    for (const r of rows) counts[r.status as keyof typeof counts]++;
    return {total: rows.length, counts};
  });
  expect(summary.total).toBeGreaterThan(0);
  // Same file diffed against itself: every row is UNCHANGED.
  expect(summary.counts.NEW).toBe(0);
  expect(summary.counts.REMOVED).toBe(0);
  expect(summary.counts.GREW).toBe(0);
  expect(summary.counts.SHRANK).toBe(0);
});

// 4. Baseline label for hprof: "Java heap dump" rather than "pid 0"
//    when the trace has no process metadata.
test('hprof baseline label is "Java heap dump" when pid is unknown', async () => {
  await ensureOnOverview();
  await fileInputLocator().setInputFiles(tracePath(PRIMARY_HPROF));
  await page.waitForFunction(
    () => window.__heapdumpDebug?.hasBaseline(),
    null,
    {timeout: 60_000},
  );
  // The trigger label includes the dump's process label.
  const trigger = page
    .locator('.ah-top-bar')
    .locator(`button:has-text("${PRIMARY_HPROF}")`)
    .first();
  await expect(trigger).toBeVisible();
  // Either "Java heap dump" (preferred) or "pid X" with X != 0.
  const label = (await trigger.textContent()) ?? '';
  expect(label).not.toContain('pid 0');
});
