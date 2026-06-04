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
import {expect} from '@playwright/test';

test('command palette keyboard navigation', async ({browser}) => {
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');

  // Open the command palette
  const omnibox = page.locator('input[ref=omnibox]');
  await omnibox.focus();
  await omnibox.fill('>');

  const commands = page.locator('.pf-omnibox-options-container');

  // Initially the first command should be highlighted.
  expect(commands.first()).toHaveClass('pf-highlighted');

  // Pressing up should highlight the last command.
  await omnibox.press('ArrowUp');
  expect(commands.last()).toHaveClass('pf-highlighted');

  // Pressing down should highlight the first command again.
  await omnibox.press('ArrowDown');
  expect(commands.first()).toHaveClass('pf-highlighted');
});
