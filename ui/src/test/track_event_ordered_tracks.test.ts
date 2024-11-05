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
  await pth.openTraceFile('track_event_ordered.pb');
});

test('load trace', async () => {
  await pth.waitForIdleAndScreenshot('loaded.png');
});

test('chronological order', async () => {
  const chronologicalGrp = pth.locateTrackGroup('Root Chronological');
  await chronologicalGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(chronologicalGrp);

  await pth.waitForIdleAndScreenshot('chronological.png');
});

test('explicit order', async () => {
  const explicitGrp = pth.locateTrackGroup('Root Explicit');
  await explicitGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(explicitGrp);

  await pth.waitForIdleAndScreenshot('explicit.png');
});

test('lexicographic tracks', async () => {
  const lexicographicGrp = pth.locateTrackGroup('Root Lexicographic');
  await lexicographicGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(lexicographicGrp);

  await pth.waitForIdleAndScreenshot('lexicographic.png');
});
