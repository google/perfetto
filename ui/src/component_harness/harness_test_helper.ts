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

// Playwright-side helpers for component tests (*.ctest.ts). Runs in node, not
// in the browser: never import this from harness/fixture code.

import {
  expect,
  type Locator,
  type Page,
  type PageAssertionsToHaveScreenshotOptions,
} from '@playwright/test';
import {formatScreenshotName} from '../test/perfetto_ui_test_helper';
import type {HarnessWindow} from './registry';

export interface MountOptions {
  readonly theme?: 'light' | 'dark';
}

// Loads component_harness.html with the given fixture and args, waits for it
// to render and returns a locator for the fixture's root element.
export async function mountFixture(
  page: Page,
  fixture: string,
  args: Readonly<Record<string, string>> = {},
  opts: MountOptions = {},
): Promise<Locator> {
  const params = new URLSearchParams({fixture, ...args});
  if (opts.theme) params.set('theme', opts.theme);
  const res = await page.goto(`/component_harness.html?${params}`);
  if (res === null || !res.ok()) {
    const status = res?.status() ?? 'no response';
    throw new Error(
      `Failed to load /component_harness.html (${status}). ` +
        `Rebuild or serve the UI with --enable-component-harness.`,
    );
  }
  await page.waitForFunction(
    () => (window as unknown as HarnessWindow).__harnessReady === true,
  );
  const harnessError = await page.evaluate(
    () => (window as unknown as HarnessWindow).__harnessError,
  );
  if (harnessError !== undefined) {
    throw new Error(harnessError);
  }
  return page.locator('#harness-root');
}

// Returns the events recorded by the fixture via record(), in order.
export async function harnessEvents(page: Page): Promise<unknown[]> {
  return page.evaluate(() => (window as unknown as HarnessWindow).__events);
}

// Asserts a screenshot of a component fixture (or locator) using the
// components project's path-based snapshotPathTemplate in playwright.config.ts.
export async function expectFixtureScreenshot(
  target: Page | Locator,
  name: string,
  opts?: PageAssertionsToHaveScreenshotOptions,
): Promise<void> {
  await expect(target).toHaveScreenshot(formatScreenshotName(name), opts);
}
