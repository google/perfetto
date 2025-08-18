// Copyright (C) 2025 The Android Open Source Project
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

import {expect, Locator, Page, test} from '@playwright/test';

import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'parallel'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
});

async function getTrackHelpButton(
  trackName: string,
  groupTrack?: Locator,
): Promise<Locator> {
  const track = pth.locateTrack(trackName, groupTrack);
  await track.scrollIntoViewIfNeeded();
  const trackButtons = track.locator('.pf-track__buttons');
  // To make sure the help button is visible.
  await trackButtons.hover();
  return trackButtons.locator('button i.pf-icon:has-text("help")');
}

[
  {
    testName: 'Cpu thread scheduling description',
    traceFile: 'api34_startup_cold.perfetto-trace',
    trackName: 'CPU Scheduling/CPU 2 Scheduling',
    screenshotName: 'cpu_scheduling_description.png',
  },
  {
    testName: 'Thread state description',
    traceFile: 'api34_startup_cold.perfetto-trace',
    groupName: 'Kernel threads',
    trackName: 'Kernel threads/kthreadd 2',
    screenshotName: 'thread_state_description.png',
  },
  {
    testName: 'Ftrace track description',
    traceFile: 'api34_startup_cold.perfetto-trace',
    groupName: 'Ftrace Events',
    trackName: 'Ftrace Events/Ftrace Track for CPU 0',
    screenshotName: 'ftrace_description.png',
  },
  {
    testName: 'Android log track description',
    traceFile: 'android_log.pb',
    trackName: 'Android logs',
    screenshotName: 'android_log_description.png',
  },
  {
    testName: 'TrackDescriptor description',
    traceFile: 'track_event_with_description.perfetto-trace',
    groupName: 'p1 5',
    trackName: 'p1 5/async',
    screenshotName: 'track_descriptor_description.png',
  },
].forEach((testCase) => {
  test(testCase.testName, async () => {
    await pth.openTraceFile(testCase.traceFile);
    let groupTrack: Locator | undefined;
    if (testCase.groupName) {
      groupTrack = pth.locateTrack(testCase.groupName);
      await groupTrack.scrollIntoViewIfNeeded();
      await pth.toggleTrackGroup(groupTrack);
    }
    const helpButton = await getTrackHelpButton(testCase.trackName, groupTrack);
    await expect(helpButton).toHaveCount(1);
    await helpButton.click();
    await pth.waitForIdleAndScreenshot(testCase.screenshotName);
  });
});
