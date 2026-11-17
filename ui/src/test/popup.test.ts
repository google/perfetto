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
// the demos on the widgets page's Popup section:
// - "Trigger Scrolled Out of View": the trigger lives in a scroll box.
// - "Trigger Hidden": the trigger's container can be set to display:none.
// Both demos are controlled popups which are open by default.

import {expect, test, type Locator, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.navigate('&enablePlugins=dev.perfetto.WidgetsPage#!/widgets/popup');
  await page.getByRole('heading', {name: 'Trigger Hidden'}).waitFor();
});

test.afterAll(async () => {
  await page.close();
});

function scrollDemoPopup(): Locator {
  return page.locator('.pf-popup.pf-popup-demo-scrolled-out');
}

function hiddenDemoPopup(): Locator {
  return page.locator('.pf-popup.pf-popup-demo-hidden');
}

// The arrow is drawn by its ::before, which sets visibility explicitly, so it
// has to be checked separately.
async function arrowVisibility(popup: Locator): Promise<string> {
  return popup
    .locator('.pf-popup-arrow')
    .evaluate((el) => getComputedStyle(el, '::before').visibility);
}

test('popup hides while its trigger is scrolled out of view', async () => {
  const scrollBox = page.locator('.pf-popup-demo-scroll-container');
  const scrollTo = (where: 'top' | 'middle' | 'bottom') =>
    scrollBox.evaluate((el, where) => {
      const max = el.scrollHeight - el.clientHeight;
      el.scrollTop = where === 'top' ? 0 : where === 'bottom' ? max : max / 2;
    }, where);

  // The demo starts scrolled to the middle with the trigger in view - but the
  // page itself scrolls too, so bring the demo into view first.
  await scrollBox.scrollIntoViewIfNeeded();
  await expect(scrollDemoPopup()).toBeVisible();
  expect(await arrowVisibility(scrollDemoPopup())).toBe('visible');

  // Scrolled out of view above...
  await scrollTo('top');
  await expect(scrollDemoPopup()).toBeHidden();
  expect(await arrowVisibility(scrollDemoPopup())).toBe('hidden');

  // ...and below.
  await scrollTo('bottom');
  await expect(scrollDemoPopup()).toBeHidden();

  // Scrolled back into view.
  await scrollTo('middle');
  await expect(scrollDemoPopup()).toBeVisible();
  expect(await arrowVisibility(scrollDemoPopup())).toBe('visible');
});

test('popup hides while its trigger has display:none', async () => {
  // Toggled with the demo's 'hideContainer' option.
  const hideContainer = page.getByText('hideContainer', {exact: true}).last();
  await hideContainer.scrollIntoViewIfNeeded();
  await expect(hiddenDemoPopup()).toBeVisible();

  await hideContainer.click();
  await expect(hiddenDemoPopup()).toBeHidden();
  expect(await arrowVisibility(hiddenDemoPopup())).toBe('hidden');

  await hideContainer.click();
  await expect(hiddenDemoPopup()).toBeVisible();
  expect(await arrowVisibility(hiddenDemoPopup())).toBe('visible');
});
