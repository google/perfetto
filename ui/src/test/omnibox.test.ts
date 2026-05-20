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

import {test} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test('command palette keyboard navigation', async ({browser}) => {
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  const omnibox = page.locator('input[ref=omnibox]');
  await omnibox.focus();
  await omnibox.fill('>');

  await pth.waitForIdleAndScreenshot('command_palette_pos_0.png');
  await omnibox.press('ArrowDown');
  await pth.waitForIdleAndScreenshot('command_palette_pos_1.png');
  await omnibox.press('ArrowUp');
  await pth.waitForIdleAndScreenshot('command_palette_pos_0.png');

  // Wrap around to the end of the list.
  await omnibox.press('ArrowUp');

  await pth.waitForIdleAndScreenshot('command_palette_pos_end-1.png');
  await omnibox.press('ArrowUp');
  await pth.waitForIdleAndScreenshot('command_palette_pos_end-2.png');
  await omnibox.press('ArrowDown');
  await pth.waitForIdleAndScreenshot('command_palette_pos_end-1.png');

  // Wrap around to the start of the list.
  await omnibox.press('ArrowDown');

  await pth.waitForIdleAndScreenshot('command_palette_pos_0.png');
});
