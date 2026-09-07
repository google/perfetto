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

import {test, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
import {
  EVENT_LATENCY_TRACK,
  SCROLL_TIMELINE_TRACK,
  SCROLL_TIMELINE_V4_TRACK,
  type TrackSpec,
} from '../plugins/org.chromium.ChromeScrollJank/tracks';

let pth: PerfettoTestHelper;
let page: Page;

test.beforeEach(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('chrome/scroll_m144.pftrace', {
    enablePlugins: 'org.chromium.ChromeScrollJank',
  });
});

test.afterEach(async () => await page.close());

async function selectPluginSlice(
  trackSpec: TrackSpec,
  eventId: number,
): Promise<void> {
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
  // would be very tedious. So we select the slice directly by its ID.
  // The IDs are stable for the given test trace (chrome/scroll_m144.pftrace).
  await page.evaluate(
    ({trackUri, eventId}) => {
      const trace = self.app.trace!;
      trace.selection.selectTrackEvent(trackUri, eventId);
    },
    {trackUri: trackSpec.uri, eventId},
  );
}

test('event_latency_track', async () => {
  const trk = pth.locateTrack(
    'Chrome Scroll Jank/Chrome Scroll Input Latencies',
  );
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('track.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Select the 'RendererCompositorQueueingDelay' stage within the first
  // janky EventLatency.
  await selectPluginSlice(EVENT_LATENCY_TRACK, 25012);
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('details_panel_stage.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Jump from the stage to the first janky EventLatency.
  await page.getByText('Parent EventLatency').click();
  await pth.waitForIdleAndScreenshot('details_panel_event_latency.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Jump from the first janky EventLatency to the corresponding scroll update.
  await page.getByText('Corresponding scroll update').click();
  await pth.waitForIdleAndScreenshot(
    'details_panel_link_to_scroll_timeline.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );

  // Go back to the first janky EventLatency and then jump the corresponding frame.
  await selectPluginSlice(EVENT_LATENCY_TRACK, 24945);
  await page
    .getByText('Frame where this was the first presented EventLatency')
    .click();
  await pth.waitForIdleAndScreenshot(
    'details_panel_link_to_scroll_timeline_v4.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );
});

test('scroll_timeline_track', async () => {
  const trk = pth.locateTrack('Chrome Scroll Jank/Chrome Scroll Timeline');
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('track.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Select the 'GenerationToBrowserMain' stage within the first inertial scroll
  // update.
  await selectPluginSlice(SCROLL_TIMELINE_TRACK, 2456);
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('details_panel_stage.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Jump from the stage to the first inertial scroll update.
  await page.getByText('Parent scroll update').click();
  await pth.waitForIdleAndScreenshot('details_panel_scroll_update.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Jump from the first inertial scroll update to the corresponding EventLatency.
  await page.getByText('Corresponding EventLatency').click();
  await pth.waitForIdleAndScreenshot(
    'details_panel_link_to_event_latency.png',
    {
      locator: page.locator('.pf-timeline-page__timeline'),
    },
  );

  // Go back to the first inertial scroll update and then jump the corresponding
  // frame.
  await selectPluginSlice(SCROLL_TIMELINE_TRACK, 2455);
  await page
    .getByText('Frame where this was the first presented scroll update')
    .click();
  await pth.waitForIdleAndScreenshot(
    'details_panel_link_to_scroll_timeline_v4.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );
});

test('scroll_timeline_v4_track', async () => {
  const trk = pth.locateTrack('Chrome Scroll Jank/Chrome Scroll Timeline v4');
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('scroll_timeline_v4_track.png', {
    locator: page.locator('.pf-timeline-page__timeline'),
  });

  // Select the 'Real scroll update input generation' stage within the second
  // janky frame.
  await selectPluginSlice(SCROLL_TIMELINE_V4_TRACK, 291);
  await trk.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_stage.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );

  // Jump from the stage to the second janky frame.
  await page.getByText('Parent frame').click();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_frame.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );

  // Jump from the second janky frame to the corresponding EventLatency.
  await page.getByText('First EventLatency in this frame').click();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_link_to_event_latency.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );

  // Go back to the second janky frame and then jump the corresponding frame.
  await selectPluginSlice(SCROLL_TIMELINE_V4_TRACK, 290);
  await page.getByText('First scroll update in this frame').click();
  await pth.waitForIdleAndScreenshot(
    'scroll_timeline_v4_details_panel_link_to_scroll_timeline.png',
    {locator: page.locator('.pf-timeline-page__timeline')},
  );
});
