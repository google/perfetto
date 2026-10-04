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

// ui_hierarchy.perfetto-trace has synthetic SystemUI state spliced into a
// real trace: UiStateEvents (scene Lockscreen -> Shade, shade_expansion
// 0 -> 0.5 -> 1, bouncer false) from SystemUI (pid 6404).

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

test('SysUI state tracks', async () => {
  const group = page
    .locator('.pf-track')
    .filter({hasText: 'com.android.systemui 6404'})
    .first();
  await pth.expandTrackGroup(group);
  for (const field of ['bouncer', 'scene', 'shade_expansion']) {
    const title = group.getByText(`SysUI State: ${field}`, {exact: true});
    await expect(title).toBeVisible();
  }
  await pth.waitForIdleAndScreenshot('tracks.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

async function clickTrackAt(title: string, frac: number) {
  const track = page
    .locator('.pf-track')
    .filter({has: page.getByText(title, {exact: true})})
    .last();
  const box = await track.boundingBox();
  const shell = await track.locator('.pf-track__shell').first().boundingBox();
  if (box === null || shell === null) throw new Error(`No track ${title}`);
  const left = shell.x + shell.width;
  await page.mouse.click(
    left + (box.x + box.width - left) * frac,
    box.y + box.height / 2,
  );
  await pth.waitForPerfettoIdle();
}

test('SysUI state values', async () => {
  const details = page.locator('.pf-details-shell');
  // scene: Lockscreen until t0+2.2s, then Shade (trace is ~7.4s long).
  await clickTrackAt('SysUI State: scene', 0.1);
  await expect(details).toContainText('Lockscreen');
  await clickTrackAt('SysUI State: scene', 0.7);
  await expect(details).toContainText('Shade');
  await pth.waitForIdleAndScreenshot('scene_selected.png');
});

test('continuous fields are counters', async () => {
  // shade_expansion (0 -> 0.5 -> 1) is drawn as a counter, not slices:
  // selecting it selects nothing, where a slice would open its details.
  await clickTrackAt('SysUI State: shade_expansion', 0.7);
  await expect(page.locator('.pf-details-shell')).toHaveCount(0);
});
