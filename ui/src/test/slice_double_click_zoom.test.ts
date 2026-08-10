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

import {expect, test, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

let pth: PerfettoTestHelper;
let page: Page;

const traceName = 'slice_double_click_zoom.pb';

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile(traceName);
});

test('double-clicking a slice zooms into it', async () => {
  const track = pth.locateTrack('Global Track Events/Slices');
  const canvas = track.locator('.pf-track__canvas');
  const box = await canvas.boundingBox();
  if (box === null) throw new Error('Track canvas bounding box is null');
  await canvas.dblclick({position: {x: box.width / 2, y: box.height / 2}});
  await pth.waitForPerfettoIdle();

  const visibleWindow = await page.evaluate(() => {
    const {start, end, duration} = self.app.trace!.timeline.visibleWindow;
    return {
      start: start.toNumber(),
      end: end.toNumber(),
      duration,
    };
  });
  expect(visibleWindow).toEqual({
    start: 4_937_500_000,
    end: 5_062_500_000,
    duration: 125_000_000,
  });

  await pth.waitForIdleAndScreenshot('slice_double_click_zoom.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});
