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

import {test, expect, Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('system-server-heap-graph-new.pftrace');
});

async function gotoHde(subpage: string = ''): Promise<void> {
  // Navigate via the hash without a full page reload so the loaded trace
  // (set up in beforeAll) is preserved.
  const hash = '#!/heapdump' + (subpage ? '/' + subpage : '');
  await page.evaluate((h) => {
    window.location.hash = h.slice(1);
  }, hash);
  await pth.waitForPerfettoIdle();
}

async function getHash(): Promise<string> {
  return page.evaluate(() => window.location.hash);
}

test('flamegraph tab is present and renders', async () => {
  await gotoHde();
  const flamegraphTab = page.locator('.pf-tabs__tab', {hasText: 'Flamegraph'});
  await expect(flamegraphTab).toBeVisible({timeout: 15_000});
  await flamegraphTab.click();
  await pth.waitForPerfettoIdle();
  // The pf-flamegraph container is rendered inside the now-open Gate.
  await expect(
    page.locator('.ah-flamegraph-view .pf-flamegraph').first(),
  ).toBeVisible({timeout: 15_000});
});

test('flamegraph URL roundtrips through the hash', async () => {
  await gotoHde('flamegraph');
  await pth.waitForPerfettoIdle();
  const hash = await getHash();
  expect(hash).toContain('flamegraph');
  await expect(
    page.locator('.ah-flamegraph-view .pf-flamegraph').first(),
  ).toBeVisible({timeout: 15_000});
});

test('flamegraph state survives a tab switch', async () => {
  // Load flamegraph first.
  await gotoHde('flamegraph');
  await pth.waitForPerfettoIdle();
  // Wait for the metric selector (filled once metrics are loaded).
  const metricSelect = page
    .locator('.ah-flamegraph-view .pf-flamegraph select')
    .first();
  await expect(metricSelect).toBeVisible({timeout: 15_000});
  // Pick the Object Count metric so we have something to verify.
  await metricSelect.selectOption({label: 'Object Count'});
  await pth.waitForPerfettoIdle();

  // Switch away to Overview, then back to Flamegraph.
  await page.locator('.pf-tabs__tab', {hasText: 'Overview'}).click();
  await pth.waitForPerfettoIdle();
  await page.locator('.pf-tabs__tab', {hasText: 'Flamegraph'}).click();
  await pth.waitForPerfettoIdle();

  // Selected metric is still Object Count.
  // The select value uses the option text, so check for substring.
  await expect(metricSelect).toHaveJSProperty('value', 'Object Count');
});

// Pick the largest object that has BOTH a BFS path_hash and a dominator
// path_hash, so a single object exercises both buttons. Robust to dump
// churn.
async function pickObjectInBothTrees(): Promise<number> {
  return await page.evaluate(async () => {
    const engine = self.app.trace!.engine;
    await engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;',
    );
    await engine.query(
      'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_class_tree;',
    );
    const r = await engine.query(`
      SELECT o.id AS id
      FROM heap_graph_object o
      JOIN _heap_graph_path_hashes p ON p.id = o.id
      JOIN _heap_graph_dominator_path_hashes dp ON dp.id = o.id
      ORDER BY o.self_size DESC LIMIT 1
    `);
    const it = r.iter({id: Number()});
    return it.valid() ? Number(it.id) : 0;
  });
}

const flamegraphView = () =>
  page.locator('.ah-flamegraph-view .pf-flamegraph').first();
const metricSelect = () =>
  page.locator('.ah-flamegraph-view .pf-flamegraph select').first();

test('View in Flamegraph (Shortest Path) pivots with Object Size metric', async () => {
  const objId = await pickObjectInBothTrees();
  expect(objId).toBeGreaterThan(0);

  await gotoHde('object_0x' + objId.toString(16));
  await pth.waitForPerfettoIdle();

  // Two "View in Flamegraph" buttons exist (one per path section); the
  // first sits in the Shortest Path section.
  const btn = page.getByRole('button', {name: 'View in Flamegraph'}).first();
  await expect(btn).toBeVisible({timeout: 15_000});
  await btn.click();
  await pth.waitForPerfettoIdle();

  // Shortest Path → BFS class tree → "Object Size" metric, pivoted.
  await expect(metricSelect()).toHaveJSProperty('value', 'Object Size');
  await expect(flamegraphView()).toContainText(/Pivot:.*\(this instance\)/, {
    timeout: 15_000,
  });
});

test('View in Flamegraph (Dominator Path) pivots with Dominated metric', async () => {
  const objId = await pickObjectInBothTrees();
  expect(objId).toBeGreaterThan(0);

  await gotoHde('object_0x' + objId.toString(16));
  await pth.waitForPerfettoIdle();

  // The second "View in Flamegraph" button sits in the Dominator Path
  // section.
  const btn = page.getByRole('button', {name: 'View in Flamegraph'}).nth(1);
  await expect(btn).toBeVisible({timeout: 15_000});
  await btn.click();
  await pth.waitForPerfettoIdle();

  // Dominator Path → dominator tree → "Dominated Object Size" metric.
  await expect(metricSelect()).toHaveJSProperty(
    'value',
    'Dominated Object Size',
  );
  await expect(flamegraphView()).toContainText(/Pivot:.*\(this instance\)/, {
    timeout: 15_000,
  });
});

test('Repeat clicking the same path button re-applies the pivot', async () => {
  // Regression: earlier code latched the consumed pathHash, so a
  // second click on the same object's button silently dropped the pivot.
  const objId = await pickObjectInBothTrees();
  expect(objId).toBeGreaterThan(0);

  await gotoHde('object_0x' + objId.toString(16));
  await pth.waitForPerfettoIdle();
  await page.getByRole('button', {name: 'View in Flamegraph'}).first().click();
  await pth.waitForPerfettoIdle();
  await expect(flamegraphView()).toContainText(/Pivot:/, {timeout: 15_000});

  // Switch the metric: this resets the view to TOP_DOWN, simulating the
  // user clearing the pivot.
  await metricSelect().selectOption({label: 'Object Count'});
  await pth.waitForPerfettoIdle();

  // Go back to the same object and click the same button again.
  await gotoHde('object_0x' + objId.toString(16));
  await pth.waitForPerfettoIdle();
  await page.getByRole('button', {name: 'View in Flamegraph'}).first().click();
  await pth.waitForPerfettoIdle();

  // The pivot must be back, and the metric must have been switched back
  // to "Object Size" (matching the BFS tree the path_hash came from).
  await expect(metricSelect()).toHaveJSProperty('value', 'Object Size');
  await expect(flamegraphView()).toContainText(/Pivot:.*\(this instance\)/, {
    timeout: 15_000,
  });
});
