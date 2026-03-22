// Copyright (C) 2024 The Android Open Source Project
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

import {test, Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
});

test('load trace', async () => {
  await pth.waitForIdleAndScreenshot('loaded.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

test('info and stats', async () => {
  await pth.navigate('#!/info');
  await pth.waitForIdleAndScreenshot('into_and_stats.png');
  await pth.navigate('#!/viewer');
  await pth.waitForIdleAndScreenshot('back_to_timeline.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

test('omnibox search', async () => {
  await pth.searchSlice('composite 572441');
  await pth.resetFocus();
  await page.keyboard.press('f');
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('search_slice.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Click on show process details in the details panel.
  await page.getByText('/system/bin/surfaceflinger [598]').click();
  await page.getByText('Show process details').click();
  await pth.waitForIdleAndScreenshot('process_details.png', {
    locator: page.locator('.pf-drawer-panel__drawer'),
  });
});

test('mark', async () => {
  await pth.searchSlice('doFrame');
  await pth.waitForPerfettoIdle();
  await pth.resetFocus();

  await page.keyboard.press('F');
  await pth.waitForPerfettoIdle();

  await page.keyboard.press('M');
  await pth.waitForPerfettoIdle();

  await pth.waitForIdleAndScreenshot(`mark.png`, {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

test('track expand and collapse', async () => {
  const trackGroup = await pth.scrollToTrack('traced_probes 1054');
  await pth.toggleTrackGroup(trackGroup);
  await pth.waitForIdleAndScreenshot('traced_probes_expanded.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Click 5 times in rapid succession.
  for (let i = 0; i < 5; i++) {
    await trackGroup.click();
    await pth.waitForPerfettoIdle(50);
  }
  await pth.waitForIdleAndScreenshot('traced_probes_compressed.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});

test('pin tracks', async () => {
  const trackGroup = await pth.scrollToTrack('traced 1055');
  await pth.toggleTrackGroup(trackGroup);
  let track = await pth.scrollToTrack('traced 1055/mem.rss');
  await pth.pinTrackUsingShellBtn(track);
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('one_track_pinned.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  track = await pth.scrollToTrack('traced 1055/traced 1055');
  await pth.pinTrackUsingShellBtn(track);
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('two_tracks_pinned.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });
});
