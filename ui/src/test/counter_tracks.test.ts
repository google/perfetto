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

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('counter_precision.pftrace');
});

async function hoverTrackAtFraction(track: Locator, fraction: number) {
  await track.scrollIntoViewIfNeeded();
  const box = ensureExists(await track.boundingBox());
  const shellBox = ensureExists(
    await track.locator('.pf-track__shell').boundingBox(),
  );
  const canvasLeft = box.x + shellBox.width;
  const canvasWidth = box.width - shellBox.width;
  await page.mouse.move(
    canvasLeft + canvasWidth * fraction,
    box.y + box.height / 2,
  );
  await pth.waitForPerfettoIdle();
}

test('counter track tooltip preserves float64 precision', async () => {
  const group = pth.locateTrack('Global Track Events');
  await pth.expandTrackGroup(group);

  const tooltip = page.locator('.pf-cursor-tooltip');

  // These tests verify that the tooltip for counter tracks preserves the full
  // precision of the underlying counter values.
  // Note: This depends on the locale being set to one that uses commas as
  // thousands separators.

  await pth.step('precise fractional doubles', async () => {
    const track = pth.locateTrack('Global Track Events/Precise Doubles');
    await hoverTrackAtFraction(track, 0.1);
    await expect(tooltip).toHaveText('10,000,000.123');

    await hoverTrackAtFraction(track, 0.5);
    await expect(tooltip).toHaveText('123,456,789.456');

    await hoverTrackAtFraction(track, 0.9);
    await expect(tooltip).toHaveText('16,777,217.5');
  });

  await pth.step('large exact integers', async () => {
    const track = pth.locateTrack('Global Track Events/Large Exact Integers');
    await hoverTrackAtFraction(track, 0.125);
    await expect(tooltip).toHaveText('4,000,000,000,001');

    await hoverTrackAtFraction(track, 0.375);
    await expect(tooltip).toHaveText('4,000,000,000,002');

    await hoverTrackAtFraction(track, 0.625);
    await expect(tooltip).toHaveText('6,000,000,000,123');

    await hoverTrackAtFraction(track, 0.875);
    await expect(tooltip).toHaveText('8,000,000,000,456');
  });
});
