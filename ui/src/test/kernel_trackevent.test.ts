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

import {test, Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}, _testInfo) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('ftrace_kernel_trackevent.pftrace');
});

test('kernel trackevent tracks', async () => {
  // cpu-scoped tracks
  const cpuGrp = pth.locateTrack('CPU');
  await cpuGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(cpuGrp);

  // custom-scoped tracks
  const kernelGrp = pth.locateTrack('Kernel');
  await kernelGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(kernelGrp);

  const kernelTrkGrp = pth.locateTrack('Kernel/Kernel track events', kernelGrp);
  await kernelTrkGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(kernelTrkGrp);

  // process-scoped tracks
  const processGrp = pth.locateTrack('Process 535');
  await processGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(processGrp);

  // thread-scoped tracks
  const ThreadGrp = pth.locateTrack('Thread 537');
  await ThreadGrp.scrollIntoViewIfNeeded();
  await pth.toggleTrackGroup(ThreadGrp);

  await pth.waitForIdleAndScreenshot('ftrace_kernel_trackevent.png');
});
