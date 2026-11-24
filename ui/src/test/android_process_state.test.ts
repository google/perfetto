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
  await pth.openTraceFile('android_process_oom_score.pftrace');
});

async function trackCanvasBounds(track: Locator) {
  await track.scrollIntoViewIfNeeded();
  const box = ensureExists(await track.boundingBox());
  const shellBox = ensureExists(
    await track.locator('.pf-track__shell').boundingBox(),
  );
  return {
    left: box.x + shellBox.width,
    width: box.width - shellBox.width,
    top: box.y,
    height: box.height,
  };
}

async function hoverTrackAtFraction(track: Locator, fraction: number) {
  const {left, width, top, height} = await trackCanvasBounds(track);
  await page.mouse.move(left + width * fraction, top + height / 2);
  await pth.waitForPerfettoIdle();
}

async function hasStepFillAtFraction(
  track: Locator,
  fraction: number,
): Promise<boolean> {
  const {left, width, top, height} = await trackCanvasBounds(track);
  const strip = await page.screenshot({
    clip: {
      x: Math.round(left + width * fraction),
      y: Math.round(top + 2),
      width: 1,
      height: Math.round(height - 4),
    },
  });
  return page.evaluate(async (b64) => {
    const img = new Image();
    img.src = `data:image/png;base64,${b64}`;
    await img.decode();
    const canvas = new OffscreenCanvas(img.width, img.height);
    const ctx = canvas.getContext('2d');
    if (ctx === null) throw new Error('Failed to get 2d context');
    ctx.drawImage(img, 0, 0);
    const {data} = ctx.getImageData(0, 0, img.width, img.height);
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] !== data[i + 1] || data[i + 1] !== data[i + 2]) return true;
    }
    return false;
  }, strip.toString('base64'));
}

test('per-process OOM score counter track stops at process exit', async () => {
  await pth.expandTrackGroup(pth.locateTrack('Process states'));
  await pth.expandTrackGroup(pth.locateTrack('Process states/By process'));
  await pth.expandTrackGroup(
    pth.locateTrack('Process states/By process/app 200'),
  );
  await pth.expandTrackGroup(
    pth.locateTrack('Process states/By process/sys 100'),
  );

  const tooltip = page.locator('.pf-cursor-tooltip');

  // Trace spans [1000, 9000]; pid 200 has samples at 2000 (999) and 4000 (200)
  // and exits at 5000.
  const exitedTrack = pth.locateTrack(
    'Process states/By process/app 200/OOM score',
  );
  expect(await hasStepFillAtFraction(exitedTrack, 0.4375)).toBe(true);
  expect(await hasStepFillAtFraction(exitedTrack, 0.75)).toBe(false);

  await hoverTrackAtFraction(exitedTrack, 0.4375);
  await expect(tooltip).toHaveText('200');

  await hoverTrackAtFraction(exitedTrack, 0.75);
  await expect(tooltip).toBeHidden();

  const liveTrack = pth.locateTrack(
    'Process states/By process/sys 100/OOM score',
  );
  expect(await hasStepFillAtFraction(liveTrack, 0.75)).toBe(true);
  await hoverTrackAtFraction(liveTrack, 0.75);
  await expect(tooltip).toHaveText('-900');
});
