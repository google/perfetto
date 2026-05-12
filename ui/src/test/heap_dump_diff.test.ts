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

// End-to-end tests for the Heapdump Explorer baseline / diff mode.
//
// Test fixtures (from upstream test/data/, downloaded by tools/test_data):
//   - system-server-heap-graph.pftrace      ← baseline
//   - system-server-heap-graph-new.pftrace  ← current (later snapshot,
//                                              same process)
//   - test-dump.hprof                       ← raw hprof for the hprof test
//   - api34_startup_cold.perfetto-trace     ← non-heap trace for rejection
//
// The pftrace pair is two real heap dumps of the same process taken at
// different times — perfect for asserting non-zero deltas. The fixtures are
// already part of the upstream repo, so no new files are committed for tests.
//
// UI selectors map to Perfetto widgets:
//   - "Diff against another trace" → Primary CTA button inside
//                                the Overview tab when no baseline is loaded.
//   - Clear baseline           → Button[aria-label="Clear active baseline"]
//                                in the top bar's baseline controls.
//   - Mode toggle              → SegmentedButtons (.pf-segmented-buttons) with
//                                buttons labelled Diff / Current / Baseline.
//   - Diff status text         → Plain coloured uppercase span text inside
//                                a DataGrid cell. We assert by *text*
//                                (`text=GREW`, etc.) — there is no pill class.
//   - Inline error             → Callout (.pf-callout.pf-intent-danger).

import {test, expect, Page} from '@playwright/test';
import path from 'path';
import fs from 'fs';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
// Side-effect imports: bring the global `Window.__heapdumpDebug` and
// `Window.__heapdumpDiff` augmentations into scope so
// `page.evaluate(() => window.__heapdumpDebug)` etc. are typed.
import '../plugins/com.android.HeapDumpExplorer/baseline/state';
import '../plugins/com.android.HeapDumpExplorer/diff/diff_debug';

test.describe.configure({mode: 'serial'});

const PRIMARY_TRACE = 'system-server-heap-graph-new.pftrace';
const BASELINE_TRACE = 'system-server-heap-graph.pftrace';
const HPROF_TRACE = 'test-dump.hprof';
const NON_HEAP_TRACE = 'api34_startup_cold.perfetto-trace';

let pth: PerfettoTestHelper;
let page: Page;

function tracePath(name: string): string {
  const cwd = process.cwd();
  const parts = ['test', 'data', name];
  if (cwd.endsWith('/ui')) parts.unshift('..');
  const p = path.join(...parts);
  if (!fs.existsSync(p)) {
    throw new Error(`Missing test fixture ${p} (cwd=${cwd})`);
  }
  return p;
}

/**
 * Locator for the hidden file input that powers the "Load baseline" button.
 * Only present on the page when no baseline is currently loaded (the
 * LoadBaselineButton lives in the Overview tab).
 */
function fileInputLocator() {
  return page.locator('input[type=file][aria-hidden="true"]');
}

async function ensureOnOverview(): Promise<void> {
  await page.locator('.pf-tabs__tab:has-text("Overview")').first().click();
  await pth.waitForPerfettoIdle();
}

async function loadBaseline(name: string): Promise<void> {
  // The "Load baseline" affordance lives in the Overview tab when no
  // baseline is currently loaded.
  await ensureOnOverview();
  await fileInputLocator().setInputFiles(tracePath(name));
  await page.waitForFunction(
    () => window.__heapdumpDebug?.hasBaseline(),
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();
}

// Page errors and console errors collected during a test, asserted to be
// empty in the dedicated console-clean test. Reset by beforeEach.
let pageErrors: string[] = [];
let consoleErrors: string[] = [];

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  page.on('pageerror', (err) => {
    pageErrors.push(err.message);
  });
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  await pth.openTraceFile(PRIMARY_TRACE);
  // Hash-only navigation so we don't reload the page (which would drop the
  // in-memory trace; pth.navigate uses page.goto which is a full reload).
  await page.evaluate(() => {
    window.location.hash = '#!/heapdump';
  });
  await pth.waitForPerfettoIdle();
});

test.beforeEach(() => {
  pageErrors = [];
  consoleErrors = [];
});

test.afterEach(async () => {
  // Reset baseline pool between tests so each starts clean. "Remove all
  // baseline traces" disposes every pooled engine — equivalent to the
  // pre-pool "close baseline" but tear-down semantics rather than
  // deselect-only.
  await page.evaluate(() => {
    document
      .querySelector<HTMLButtonElement>(
        'button[aria-label="Remove all baseline traces"]',
      )
      ?.click();
  });
});

// 1. Overview tab exposes the Load baseline button when no baseline is loaded.
test('overview shows Load baseline button initially', async () => {
  await ensureOnOverview();
  await expect(
    page.locator('button:has-text("Diff against another trace")').first(),
  ).toBeVisible();
  // No slim baseline-status callout when nothing is loaded.
  await expect(
    page.locator('button[aria-label="Clear active baseline"]'),
  ).toHaveCount(0);
  // Debug surface is wired up.
  const hasBaseline = await page.evaluate(() =>
    window.__heapdumpDebug?.hasBaseline(),
  );
  expect(hasBaseline).toBe(false);
});

// 2. Load baseline → diff mode is active by default and the slim status
//    header is visible with a Close button.
test('loading a baseline activates diff mode', async () => {
  await loadBaseline(BASELINE_TRACE);
  const filename = await page.evaluate(() =>
    window.__heapdumpDebug!.baselineFilename(),
  );
  expect(filename).toBe(BASELINE_TRACE);
  const mode = await page.evaluate(() => window.__heapdumpDebug!.mode());
  expect(mode).toBe('diff');
  // Close button appears once a baseline is loaded.
  await expect(
    page.locator('button[aria-label="Clear active baseline"]'),
  ).toBeVisible();
});

// 3. Same trace as primary AND baseline → every status should be UNCHANGED
//    (rendered as empty span); no GREW/SHRANK/NEW/REMOVED text in the grid.
test('same trace as baseline produces all-zero deltas', async () => {
  test.setTimeout(120_000);
  await loadBaseline(PRIMARY_TRACE); // same file as primary
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  // Wait for the diff to finish: the heading "Classes diff" appears with a
  // resolved row count (no leading spinner anymore).
  await page
    .locator('.ah-view-heading:has-text("Classes diff")')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  // Wait for at least one DataGrid row to be attached so we know rendering
  // completed (otherwise an empty grid trivially passes).
  await page
    .locator('.pf-data-grid .pf-grid__row')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  // Status text cells in DataGrid: count any GREW/SHRANK/NEW/REMOVED.
  const nonUnchanged = await page
    .locator('.pf-data-grid')
    .locator('text=/^(GREW|SHRANK|NEW|REMOVED)$/')
    .count();
  expect(nonUnchanged).toBe(0);
});

// 4. Different traces → at least one row appears with a non-UNCHANGED status.
test('different baseline produces visible diffs', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  // The two-phase query against 1M+ heap_graph_object rows can take ~30s.
  // Wait for any GREW/SHRANK/NEW/REMOVED text inside a DataGrid cell.
  await page
    .locator('.pf-data-grid')
    .locator('text=/^(GREW|SHRANK|NEW|REMOVED)$/')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  const nonUnchanged = await page
    .locator('.pf-data-grid')
    .locator('text=/^(GREW|SHRANK|NEW|REMOVED)$/')
    .count();
  expect(nonUnchanged).toBeGreaterThan(0);

  // Parity-style invariants on the merged DiffRow array (exposed via the
  // window.__heapdumpDiff debug API). The diff classifier must be self-
  // consistent: every row has a status drawn from a fixed enum, classes
  // present in only one side are NEW/REMOVED (not GREW/SHRANK), and the
  // sum of status buckets equals the row count. These are the same rules
  // a hand-written diff against ahat would expect to hold.
  await page.waitForFunction(
    () => (window.__heapdumpDiff?.gen('classes') ?? 0) > 0,
    null,
    {timeout: 30_000},
  );
  const summary = await page.evaluate(() => {
    const rows = window.__heapdumpDiff!.rows('classes')!;
    const counts = {NEW: 0, REMOVED: 0, GREW: 0, SHRANK: 0, UNCHANGED: 0};
    let presenceViolations = 0;
    for (const r of rows) {
      counts[r.status as keyof typeof counts]++;
      const bAbs = r._b_reachable_obj_count;
      const cAbs = r._c_reachable_obj_count;
      // If status is NEW, the baseline side must be missing (encoded as
      // null on _b_*). If REMOVED, the current side must be missing.
      if (r.status === 'NEW' && bAbs !== null) presenceViolations++;
      if (r.status === 'REMOVED' && cAbs !== null) presenceViolations++;
    }
    return {total: rows.length, counts, presenceViolations};
  });
  expect(summary.total).toBeGreaterThan(0);
  expect(summary.presenceViolations).toBe(0);
  // Bucket sum must equal total.
  const sum = Object.values(summary.counts).reduce((a, b) => a + b, 0);
  expect(sum).toBe(summary.total);
  // Different traces should produce at least one non-UNCHANGED row in the
  // merged data structure too.
  expect(summary.counts.UNCHANGED).toBeLessThan(summary.total);
});

// 5. HPROF baseline loads as well as a pftrace baseline.
test('hprof baseline loads', async () => {
  await loadBaseline(HPROF_TRACE);
  const filename = await page.evaluate(() =>
    window.__heapdumpDebug!.baselineFilename(),
  );
  expect(filename).toBe(HPROF_TRACE);
});

// 6. A non-heap trace as baseline is rejected with an inline error Callout.
test('non-heap trace rejected with inline error', async () => {
  await ensureOnOverview();
  await fileInputLocator().setInputFiles(tracePath(NON_HEAP_TRACE));
  // Wait for the danger-intent Callout (rendered both inside Overview and
  // in the slim header above the tabs).
  await page
    .locator('.pf-callout.pf-intent-danger')
    .first()
    .waitFor({state: 'visible', timeout: 60_000});
  const err = await page
    .locator('.pf-callout.pf-intent-danger')
    .first()
    .innerText();
  expect(err).toMatch(/no Java heap data|heap dump/i);
  // Baseline should NOT be set.
  const hasBaseline = await page.evaluate(() =>
    window.__heapdumpDebug!.hasBaseline(),
  );
  expect(hasBaseline).toBe(false);
});

// 7. Tearing down the pool returns the page to the load-button state.
test('removing all baselines disposes engines and reverts header', async () => {
  await loadBaseline(BASELINE_TRACE);
  // "Remove all baseline traces" disposes every pooled engine and clears
  // the active selection — the pre-pool "close baseline" UX condensed into
  // the explicit teardown gesture.
  await page.locator('button[aria-label="Remove all baseline traces"]').click();
  await page.waitForFunction(
    () =>
      window.__heapdumpDebug!.hasBaseline() === false &&
      window.__heapdumpDebug!.poolSize() === 0,
    null,
    {timeout: 5_000},
  );
  // Discovery CTA is back inside the Overview tab.
  await ensureOnOverview();
  await expect(
    page.locator('button:has-text("Diff against another trace")').first(),
  ).toBeVisible();
});

// 8. Mode toggle: switching to "Current" shows the existing single-engine
//    Classes view (no diff Δ columns).
test('Current-only mode hides diff columns', async () => {
  await loadBaseline(BASELINE_TRACE);
  // Switch the SegmentedButtons mode toggle to "Current".
  await page.locator('.pf-segmented-button:has-text("Current")').click();
  await pth.waitForPerfettoIdle();
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await pth.waitForPerfettoIdle();
  // The diff view's view-heading reads "Classes diff (N classes)";
  // the single-engine ClassesView heading reads "Classes (M)".
  // If Current-only correctly swapped views, no "Classes diff" header
  // should be visible anywhere on the page.
  const visibleDiffHeader = await page
    .locator('.ah-view-heading:visible:has-text("Classes diff")')
    .count();
  expect(visibleDiffHeader).toBe(0);
  // The single-engine "Classes" heading is visible.
  await page
    .locator('.ah-view-heading:visible')
    .first()
    .waitFor({timeout: 30_000});
});

// 9. Smoke for each diff-capable tab: navigate, ensure no error renders.
test('every diff-capable tab renders without error', async () => {
  test.setTimeout(300_000);
  await loadBaseline(BASELINE_TRACE);
  for (const tab of [
    'Overview',
    'Classes',
    'Strings',
    'Arrays',
    'Bitmaps',
    'Dominators',
  ]) {
    await page.locator(`.pf-tabs__tab:has-text("${tab}")`).click();
    // Give the diff query time to complete; we don't wait for content
    // because Strings on a 1M-object trace can be slow.
    await page.waitForTimeout(15_000);
    // No error/empty state with "Failed".
    const failed = await page
      .locator('.pf-empty-state:has-text("Failed")')
      .count();
    expect(failed, `tab ${tab} renders an error`).toBe(0);
  }
});

// 10. Overview tab MUST switch to the diff layout once the baseline overview
//     has finished loading. The 'Overview diff' heading proves the unified
//     view received `baselineOverview` and rendered the diff branch.
test('Overview tab swaps to diff layout when baseline loads', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  await ensureOnOverview();
  // The view-heading text flips from "Overview" → "Overview diff" once
  // baselineOverview is computed and threaded in. Use :visible to skip
  // any hidden tab-content headings the Tabs widget may keep around.
  await expect(
    page.locator('.ah-view-heading:visible:has-text("Overview diff")'),
  ).toBeVisible({timeout: 120_000});
  // The General Information card now has Baseline / Current / Δ columns.
  // Same :visible discipline; also a longer toContainText timeout in case
  // the overview rerender cascades over multiple frames.
  const infoCard = page
    .locator('.ah-card:visible:has-text("General Information")')
    .first();
  await expect(infoCard).toContainText('Baseline', {timeout: 30_000});
  await expect(infoCard).toContainText('Current', {timeout: 5_000});
  // Bytes Retained by Heap card too.
  await expect(
    page.locator('.ah-card:visible:has-text("Bytes Retained by Heap")').first(),
  ).toContainText('Δ Total', {timeout: 30_000});
});

// 11a. Multi-trace pool: pool grows to two distinct baseline traces; the
//      first is auto-picked, the second is just queued. The active dump's
//      identity comes from the first trace; pool size reflects both.
test('multi-trace pool grows when adding a second baseline', async () => {
  test.setTimeout(180_000);
  // 1) First baseline (auto-picks since the pool was empty + single-dump).
  await loadBaseline(BASELINE_TRACE);
  expect(await page.evaluate(() => window.__heapdumpDebug!.poolSize())).toBe(1);
  expect(
    await page.evaluate(() => window.__heapdumpDebug!.baselineFilename()),
  ).toBe(BASELINE_TRACE);

  // 2) Add a second baseline (the hprof). The header keeps the file
  //    input mounted; we don't need to navigate the popup — the same
  //    hidden input services the Overview CTA *and* the popup's
  //    "Add baseline trace…" menu item. Setting files directly is
  //    representative because that's what either path would do once
  //    the OS picker resolves.
  await fileInputLocator().setInputFiles(tracePath(HPROF_TRACE));
  await page.waitForFunction(
    () => window.__heapdumpDebug!.poolSize() === 2,
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();

  // 3) Active baseline did NOT change — we already had one, so the
  //    auto-pick rule (only when active===null && dumps.length===1)
  //    didn't fire for the second trace.
  expect(
    await page.evaluate(() => window.__heapdumpDebug!.baselineFilename()),
  ).toBe(BASELINE_TRACE);
});

// 11b. The popup must list both pooled traces' titles so the user can
//      see what's loaded and pick a dump from either.
test('popup lists every pooled baseline trace', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  await fileInputLocator().setInputFiles(tracePath(HPROF_TRACE));
  await page.waitForFunction(
    () => window.__heapdumpDebug!.poolSize() === 2,
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();
  // Open the popup by clicking the trigger (its label is the active
  // baseline's "title · process".
  await page
    .locator('.ah-top-bar')
    .locator('button:has-text("system-server-heap-graph.pftrace")')
    .first()
    .click();
  // Both trace section headings appear (they're MenuItems with
  // folder_open icons; we just match the title text).
  await expect(
    page.locator('.pf-menu-item:has-text("system-server-heap-graph.pftrace")'),
  ).toHaveCount(2); // section heading + Remove "..." item
  await expect(
    page.locator('.pf-menu-item:has-text("test-dump.hprof")'),
  ).toHaveCount(2); // same for second trace
  // Add baseline trace… is always last.
  await expect(
    page.locator('.pf-menu-item:has-text("Add baseline trace…")'),
  ).toBeVisible();
});

// 11b. Pretty column titles: the diff Classes view headers are
//      human-readable ("Δ Retained", "Current Retained") not the raw
//      snake_case field names ("Δ dominated_size_bytes"). Catches a
//      regression where the schema would fall back to field names.
test('Classes diff has human-readable column titles', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await page
    .locator('.ah-view-heading:has-text("Classes diff")')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  const headerText = (
    await page.locator('.pf-data-grid .pf-grid-header-cell').allInnerTexts()
  ).join(' | ');
  // We just need to assert that *some* nice header is visible AND
  // that no raw snake_case header leaked through.
  expect(headerText).toMatch(/Δ Retained/);
  expect(headerText).toMatch(/Current Retained/);
  expect(headerText).toMatch(/Baseline Retained/);
  expect(headerText).not.toMatch(/dominated_size_bytes/);
  expect(headerText).not.toMatch(/reachable_obj_count/);
});

// 11c. Top bar layout: when a baseline is loaded the .ah-top-bar row
//      is visible, has the expected children (label, popup trigger,
//      mode toggle, clear/dispose buttons), and is a single horizontal
//      row (height < 80px on a 1080p viewport).
test('top bar renders as a single styled row when baseline loaded', async () => {
  await loadBaseline(BASELINE_TRACE);
  const bar = page.locator('.ah-top-bar:not(.ah-top-bar--hidden)');
  await expect(bar).toBeVisible();
  const box = await bar.boundingBox();
  expect(box, 'top bar should have a layout box').not.toBeNull();
  expect(box!.height, 'top bar should be a single row').toBeLessThan(80);
  // Required pieces are inside.
  await expect(bar.locator('text=Baseline:')).toBeVisible();
  await expect(bar.locator('.pf-segmented-buttons')).toBeVisible();
  await expect(
    bar.locator('button[aria-label="Clear active baseline"]'),
  ).toBeVisible();
  await expect(
    bar.locator('button[aria-label="Remove all baseline traces"]'),
  ).toBeVisible();
});

// 11d. Status colour cues: the GREW/SHRANK badges render with the
//      expected --pf-color-* tokens. Asserts colour is the dominant
//      semantic so colour-blind users have the aria-label as backup.
test('Status badges use Perfetto color tokens', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  // The .ah-status-text span only renders for non-UNCHANGED rows.
  // Wait for at least one to attach.
  await page
    .locator('.pf-data-grid .ah-status-text')
    .first()
    .waitFor({state: 'attached', timeout: 90_000});
  // Find every status badge in the grid and partition by aria-label
  // (the source-of-truth attribute we set in the renderer). We do this
  // in one page.evaluate so we don't pay for many roundtrips on a long
  // grid, and so we get a single deterministic snapshot.
  const samples = await page.evaluate(() => {
    const out: Array<{label: string; aria: string; color: string}> = [];
    const cells = document.querySelectorAll<HTMLElement>(
      '.pf-data-grid .ah-status-text',
    );
    for (const c of cells) {
      out.push({
        label: (c.textContent ?? '').trim(),
        aria: c.getAttribute('aria-label') ?? '',
        color: getComputedStyle(c).color,
      });
      if (out.length >= 200) break;
    }
    return out;
  });
  expect(samples.length, 'no status badges in grid').toBeGreaterThan(0);
  // The DataGrid is virtualized — only currently-visible status cells
  // are in the DOM (sorted Δ-DESC, so usually NEW or large GREW). Map
  // each visible status to its expected colour invariant and assert.
  const expects: Record<string, (rgb: number[]) => boolean> = {
    NEW: ([r, g, b]) => r > 100 && g > 100 && b < 100, // amber/warning
    GREW: ([r, g, b]) => r > g && r > b, // red/danger
    SHRANK: ([r, g, b]) => g > r && g > b, // green/success
    REMOVED: ([r, g, b]) => Math.abs(r - g) < 30 && Math.abs(g - b) < 30, // muted gray
  };
  let checked = 0;
  for (const s of samples) {
    const pred = expects[s.label];
    if (pred === undefined) continue;
    const rgb = (s.color.match(/\d+/g) ?? []).map(Number);
    expect(
      pred(rgb),
      `${s.label} colour ${s.color} fails its hue invariant`,
    ).toBe(true);
    checked++;
  }
  expect(
    checked,
    `no recognised status badges in sample; got: ${samples.map((s) => s.label).join(',')}`,
  ).toBeGreaterThan(0);
});

// 11e. Multi-trace pool switch: load two baselines, programmatically pick
//      the second, verify the active baseline flips and the diff content
//      actually changes. Uses a window-exposed test helper so we don't
//      have to fight DataGrid header order or popup ordering.
test('switching active baseline between two pooled traces re-renders diff', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  await fileInputLocator().setInputFiles(tracePath(HPROF_TRACE));
  await page.waitForFunction(
    () => window.__heapdumpDebug!.poolSize() === 2,
    null,
    {timeout: 60_000},
  );
  await pth.waitForPerfettoIdle();

  // Initial state: BASELINE_TRACE is active.
  expect(
    await page.evaluate(() => window.__heapdumpDebug!.baselineFilename()),
  ).toBe(BASELINE_TRACE);

  // Render the Classes diff once with BASELINE_TRACE so a snapshot is
  // published.
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
  const genBefore = await page.evaluate(
    () => window.__heapdumpDiff!.gen('classes')!,
  );
  const totalBefore = await page.evaluate(
    () => window.__heapdumpDiff!.rows('classes')!.length,
  );

  // Switch to the hprof baseline.
  await page.evaluate(() =>
    window.__heapdumpDebug!.pickBaseline('test-dump.hprof'),
  );
  expect(
    await page.evaluate(() => window.__heapdumpDebug!.baselineFilename()),
  ).toBe(HPROF_TRACE);

  // Wait for a fresh classes snapshot to land — the gen counter must
  // strictly advance because the diff view remounted (tabsKey change)
  // and re-published.
  await page.waitForFunction(
    (g) => (window.__heapdumpDiff?.gen('classes') ?? 0) > g,
    genBefore,
    {timeout: 90_000},
  );
  const totalAfter = await page.evaluate(
    () => window.__heapdumpDiff!.rows('classes')!.length,
  );
  // The two baselines have different class sets — totals should differ.
  expect(totalAfter).not.toBe(totalBefore);
});

// 11f. Mid-flight cancellation: start a Classes diff, immediately clear
//      the baseline before it can finish. The published snapshot for
//      'classes' must be empty (cleared by clearActiveBaseline → the
//      pending fetch's snapshot guard aborts the publish).
test('clearing baseline mid-flight aborts the diff cleanly', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  // Yield one frame so the diff view's load() begins.
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(r)));
  // Clear before the query completes.
  await page.locator('button[aria-label="Clear active baseline"]').click();
  // After clear, the published rows are empty; gen also resets to 0
  // (clearDiffRows wipes the snapshot map).
  await page.waitForFunction(
    () => window.__heapdumpDebug!.hasBaseline() === false,
    null,
    {timeout: 5_000},
  );
  expect(await page.evaluate(() => window.__heapdumpDiff!.gen('classes'))).toBe(
    0,
  );
  expect(
    await page.evaluate(() => window.__heapdumpDiff!.rows('classes')),
  ).toBeNull();
  // No error appears — the abort is silent.
  expect(
    await page.locator('.pf-empty-state:visible:has-text("Failed")').count(),
  ).toBe(0);
});

// 11g. Re-load a previously-cleared trace's same file. The pool stays
//      with one trace BUT it's now a fresh entry (new engine, new id);
//      the active dump auto-picks. Verifies clear-vs-remove distinction
//      and that the auto-pick rule still works after a clear.
test('clear + reload same baseline file replaces the pooled entry', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  expect(await page.evaluate(() => window.__heapdumpDebug!.poolSize())).toBe(1);

  // Clear (pool keeps the trace) and reload the same file (now we have
  // 2 entries, both with title BASELINE_TRACE).
  await page.locator('button[aria-label="Clear active baseline"]').click();
  await page.waitForFunction(
    () => window.__heapdumpDebug!.hasBaseline() === false,
    null,
    {timeout: 5_000},
  );
  await fileInputLocator().setInputFiles(tracePath(BASELINE_TRACE));
  await page.waitForFunction(
    () => window.__heapdumpDebug!.poolSize() === 2,
    null,
    {timeout: 60_000},
  );
  // Auto-pick fired this time (active was null + single-dump trace).
  await page.waitForFunction(
    () => window.__heapdumpDebug!.hasBaseline() === true,
    null,
    {timeout: 5_000},
  );
  expect(
    await page.evaluate(() => window.__heapdumpDebug!.baselineFilename()),
  ).toBe(BASELINE_TRACE);
});

// 11h. Switching tabs while a diff is loading does not crash, and the
//      destination tab eventually shows its data.
test('rapid tab switching during diff load is safe', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  // Visit each diff-capable tab in quick succession. No waits between
  // clicks — we want overlapping in-flight fetches.
  for (const tab of ['Classes', 'Strings', 'Arrays', 'Bitmaps', 'Dominators']) {
    await page.locator(`.pf-tabs__tab:has-text("${tab}")`).click();
  }
  // Land on Classes and let it finish.
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await page
    .locator('.ah-view-heading:has-text("Classes diff")')
    .first()
    .waitFor({state: 'attached', timeout: 120_000});
  await page.waitForFunction(
    () => (window.__heapdumpDiff?.gen('classes') ?? 0) > 0,
    null,
    {timeout: 90_000},
  );
  // No errors anywhere on the page.
  expect(
    await page.locator('.pf-empty-state:visible:has-text("Failed")').count(),
  ).toBe(0);
});

// 11i. Loading the SAME file as both primary (already loaded) and baseline
//      yields the all-UNCHANGED contract. Specifically, every status
//      bucket except UNCHANGED is empty in the published rows.
test('same trace as primary + baseline → only UNCHANGED rows', async () => {
  test.setTimeout(120_000);
  await loadBaseline(PRIMARY_TRACE); // same file as primary
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
  expect(summary.counts.NEW).toBe(0);
  expect(summary.counts.REMOVED).toBe(0);
  expect(summary.counts.GREW).toBe(0);
  expect(summary.counts.SHRANK).toBe(0);
  expect(summary.counts.UNCHANGED).toBe(summary.total);
});

// 11j. Mode toggle "Baseline" — show the baseline trace's data using
//      the single-engine views (no Δ columns, no diff math). The
//      heading must NOT contain "diff".
test('Baseline-only mode shows baseline data via single-engine views', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await page.locator('.pf-segmented-button:has-text("Baseline")').click();
  await pth.waitForPerfettoIdle();
  await page.locator('.pf-tabs__tab:has-text("Classes")').click();
  await pth.waitForPerfettoIdle();
  // No "Classes diff" heading.
  expect(
    await page
      .locator('.ah-view-heading:visible:has-text("Classes diff")')
      .count(),
  ).toBe(0);
  // Single-engine "Classes" heading IS visible.
  await expect(page.locator('.ah-view-heading:visible').first()).toContainText(
    /^Classes/,
    {timeout: 30_000},
  );
  // The mode debug API agrees.
  expect(await page.evaluate(() => window.__heapdumpDebug!.mode())).toBe(
    'baseline',
  );
});

// 12. No uncaught page errors or console.error across loading + tab nav.
//     Catches regressions where a missing import / runtime crash would only
//     surface as a console scribble and an empty card.
// Wait for the pool to reach `expected` traces. The base `loadBaseline`
// helper waits on hasBaseline() which goes true on the first load and
// stays true through subsequent loads — this waits on poolSize change.
async function waitForPoolSize(expected: number): Promise<void> {
  await page.waitForFunction(
    (n) => window.__heapdumpDebug?.poolSize() === n,
    expected,
    {timeout: 60_000},
  );
}

// Edge: removing a non-active pooled baseline preserves the active
// selection.
test('removing a non-active pooled trace keeps the active selection', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  await waitForPoolSize(1);
  await fileInputLocator().setInputFiles(tracePath(HPROF_TRACE));
  await waitForPoolSize(2);
  await pth.waitForPerfettoIdle();
  // Switch active to the hprof trace.
  await page.evaluate(
    (title) => window.__heapdumpDebug?.pickBaseline(title),
    HPROF_TRACE,
  );
  expect(
    await page.evaluate(() => window.__heapdumpDebug?.baselineFilename()),
  ).toBe(HPROF_TRACE);
  // Remove the non-active trace by clicking its row's "Remove …" item.
  await page.locator('.ah-top-bar button:has-text("·")').first().click();
  await page.locator(`text=Remove ${BASELINE_TRACE}`).click();
  expect(
    await page.evaluate(() => window.__heapdumpDebug?.baselineFilename()),
  ).toBe(HPROF_TRACE);
  expect(await page.evaluate(() => window.__heapdumpDebug?.poolSize())).toBe(1);
});

// Edge: removing the active pooled trace clears the selection but
// leaves the pool with the remaining trace.
test('removing the active pooled trace clears the selection', async () => {
  test.setTimeout(180_000);
  await loadBaseline(BASELINE_TRACE);
  await waitForPoolSize(1);
  await fileInputLocator().setInputFiles(tracePath(HPROF_TRACE));
  await waitForPoolSize(2);
  await pth.waitForPerfettoIdle();
  const before = await page.evaluate(() =>
    window.__heapdumpDebug?.baselineFilename(),
  );
  expect(before).not.toBeNull();
  await page.locator('.ah-top-bar button:has-text("·")').first().click();
  await page.locator(`text=Remove ${before}`).click();
  expect(await page.evaluate(() => window.__heapdumpDebug?.hasBaseline())).toBe(
    false,
  );
  expect(await page.evaluate(() => window.__heapdumpDebug?.poolSize())).toBe(1);
});

test('no console errors during baseline load and tab navigation', async () => {
  test.setTimeout(120_000);
  await loadBaseline(BASELINE_TRACE);
  await ensureOnOverview();
  await page.waitForTimeout(8_000); // let baseline overview finish
  for (const tab of ['Classes', 'Strings', 'Bitmaps', 'Overview']) {
    await page.locator(`.pf-tabs__tab:has-text("${tab}")`).click();
    await page.waitForTimeout(4_000);
  }
  expect(pageErrors, 'page emitted uncaught errors').toEqual([]);
  expect(
    consoleErrors.filter(
      // Allow trace-processor's noisy benign warnings through; they're not
      // from the plugin code.
      (e) => !e.includes('TraceProcessor') && !e.includes('WebGL'),
    ),
    'plugin emitted console errors',
  ).toEqual([]);
});
