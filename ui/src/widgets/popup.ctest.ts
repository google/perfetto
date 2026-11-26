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

// Tests that popups hide themselves while their trigger isn't visible, using
// the fixtures in widgets/popup.fixture.ts. Both are open popups.

import {expect, test, type Locator} from '@playwright/test';
import {mountFixture} from '../component_harness/harness_test_helper';

// The arrow is drawn by its ::before, which sets visibility explicitly, so it
// has to be checked separately.
async function arrowVisibility(popup: Locator): Promise<string> {
  return popup
    .locator('.pf-popup-arrow')
    .evaluate((el) => getComputedStyle(el, '::before').visibility);
}

test('popup hides while its trigger is scrolled out of view', async ({
  page,
}) => {
  const root = await mountFixture(page, 'widgets/popup/scroll_container');
  const popup = page.locator('.pf-popup');
  const scrollBox = root.locator('.scroll-container');
  const scrollTo = (where: 'top' | 'middle' | 'bottom') =>
    scrollBox.evaluate((el, where) => {
      const max = el.scrollHeight - el.clientHeight;
      el.scrollTop = where === 'top' ? 0 : where === 'bottom' ? max : max / 2;
    }, where);

  // The fixture starts scrolled to the middle with the trigger in view.
  await expect(popup).toBeVisible();
  expect(await arrowVisibility(popup)).toBe('visible');

  // Scrolled out of view above...
  await scrollTo('top');
  await expect(popup).toBeHidden();
  expect(await arrowVisibility(popup)).toBe('hidden');

  // ...and below.
  await scrollTo('bottom');
  await expect(popup).toBeHidden();

  // Scrolled back into view.
  await scrollTo('middle');
  await expect(popup).toBeVisible();
  expect(await arrowVisibility(popup)).toBe('visible');
});

test('popup hides while its trigger has display:none', async ({page}) => {
  const root = await mountFixture(page, 'widgets/popup/hideable_container');
  const popup = page.locator('.pf-popup');
  const toggle = root.getByRole('button', {name: 'Toggle container'});

  await expect(popup).toBeVisible();

  await toggle.click();
  await expect(popup).toBeHidden();
  expect(await arrowVisibility(popup)).toBe('hidden');

  await toggle.click();
  await expect(popup).toBeVisible();
  expect(await arrowVisibility(popup)).toBe('visible');
});
