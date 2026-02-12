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

import {test, Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
import {SCROLL_TIMELINE_V4_TRACK} from '../plugins/org.chromium.ChromeScrollJank/tracks';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('chrome/scroll_m144.pftrace', {
    enablePlugins: 'org.chromium.ChromeScrollJank',
  });
});

async function selectScrollTimelineV4Slice(id: number): Promise<void> {
  // We cannot use `PerfettoTestHelper.searchSlice()` because omnibox search
  // results don't include track events (slices) created by plugins.
  //
  // We could theoretically emulate clicking on the relevant plugin slice with
  // the following code:
  //
  // ```
  // const coords = assertExists(await trk.boundingBox());
  // await page.mouse.click(coords.x + 823, coords.y + 120);
  // ```
  //
  // but it would break easily, in which case updating the coordinates manually
  // would be very tedious. So we do the following instead.
  await page.evaluate(
    async ({trackUri, id}) => {
      self.app.trace!.selection.selectTrackEvent(trackUri, id);
    },
    {trackUri: SCROLL_TIMELINE_V4_TRACK.uri, id},
  );
}

test('scroll_jank_track_group', async () => {
  const grp = pth.locateTrack('Chrome Scroll Jank');
  await grp.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('scroll_jank_track_group.png');
});

test('scroll_timeline_v4_track', async () => {
  const trk = pth.locateTrack('Chrome Scroll Jank/Chrome Scroll Timeline v4');
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('scroll_timeline_v4_track.png');

  // Select the 'Real scroll update input generation' stage within the second
  // janky frame.
  await selectScrollTimelineV4Slice(291);
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_stage.png',
  );

  // Jump from the stage to the second janky frame.
  await page.getByText('Parent frame').click();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_frame.png',
  );

  // Jump from the second janky frame to its original slice.
  await page.getByText('Original slice').click();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_original_slice.png',
  );
});
