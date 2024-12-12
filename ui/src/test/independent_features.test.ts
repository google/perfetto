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

import {test} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'parallel'});

// Test that we show a (debuggable) chip next to tracks for debuggable apps.
// Regression test for aosp/3106008 .
test('debuggable chip', async ({browser}) => {
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api32_startup_warm.perfetto-trace');
  const trackGroup = pth.locateTrackGroup(
    'androidx.benchmark.integration.macrobenchmark.test 7527',
  );
  await trackGroup.scrollIntoViewIfNeeded();
  await pth.waitForIdleAndScreenshot('track_with_debuggable_chip.png');

  await pth.toggleTrackGroup(trackGroup);
  await pth.waitForIdleAndScreenshot('track_with_debuggable_chip_expanded.png');
});

test('trace error notification', async ({browser}) => {
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('clusterfuzz_14753');
  await pth.waitForIdleAndScreenshot('error-icon.png', {
    clip: {x: 1800, y: 0, width: 150, height: 150},
  });
});
