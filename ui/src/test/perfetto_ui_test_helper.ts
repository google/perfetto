// Copyright (C) 2021 The Android Open Source Project
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

import {
  expect,
  Locator,
  Page,
  PageAssertionsToHaveScreenshotOptions,
} from '@playwright/test';
import fs from 'fs';
import path from 'path';
import {IdleDetectorWindow} from '../frontend/idle_detector_interface';
import {assertExists} from '../base/logging';
import {Size2D} from '../base/geom';
import {AppImpl} from '../core/app_impl';

// Define the locators for elements you always want to mask.
const GLOBAL_MASKS: ((page: Page) => Locator)[] = [
  // Hide the footer when running integration tests, as the version code and the
  // tiny text with pending queries can fail the screenshot diff test.
  (page) => page.locator('.pf-sidebar__footer'),
];

export class PerfettoTestHelper {
  private cachedSidebarSize?: Size2D;

  constructor(readonly page: Page) {}

  resetFocus(): Promise<void> {
    return this.page.click('.pf-sidebar img.pf-sidebar__brand');
  }

  async sidebarSize(): Promise<Size2D> {
    if (this.cachedSidebarSize === undefined) {
      const size = await this.page.locator('main > .pf-sidebar').boundingBox();
      this.cachedSidebarSize = assertExists(size);
    }
    return this.cachedSidebarSize;
  }

  async navigate(fragment: string): Promise<void> {
    await this.page.goto('/?testing=1' + fragment);
    await this.waitForPerfettoIdle();
    await this.applyTestingStyles();
    await this.page.click('body');
  }

  async openTraceFile(traceName: string, args?: {}): Promise<void> {
    args = {testing: '1', ...args};
    const qs = Object.entries(args ?? {})
      .map(([k, v]) => `${k}=${v}`)
      .join('&');
    await this.page.goto('/?' + qs);
    const file = await this.page.waitForSelector('input.trace_file', {
      state: 'attached',
    });
    await this.page.evaluate(() =>
      localStorage.setItem('dismissedPanningHint', 'true'),
    );
    const tracePath = this.getTestTracePath(traceName);
    assertExists(file).setInputFiles(tracePath);
    await this.waitForPerfettoIdle();
    await this.applyTestingStyles();
    await this.page.mouse.move(0, 0);
  }

  /**
   * Applies styles to minimize rendering differences between Mac and Linux.
   */
  private async applyTestingStyles(): Promise<void> {
    await this.page.addStyleTag({
      content: `
        body {
          -webkit-font-smoothing: antialiased !important;
          font-kerning: none !important;
        }
      `,
    });
  }

  waitForPerfettoIdle(idleHysteresisMs?: number): Promise<void> {
    return this.page.evaluate(
      async (ms) =>
        (window as {} as IdleDetectorWindow).waitForPerfettoIdle(ms),
      idleHysteresisMs,
    );
  }

  async waitForIdleAndScreenshot(
    screenshotName: string,
    opts?: PageAssertionsToHaveScreenshotOptions,
  ) {
    await this.page.mouse.move(0, 0); // Move mouse out of the way.
    await this.waitForPerfettoIdle();

    // Get instances of the global locators for the current page.
    const globalMaskLocators = GLOBAL_MASKS.map((getLocator) =>
      getLocator(this.page),
    );

    // Combine global masks with any masks specific to this test call.
    const allMasks = [...globalMaskLocators, ...(opts?.mask || [])];

    // Call the original expect with the combined masks.
    await expect.soft(this.page).toHaveScreenshot(screenshotName, {
      ...opts,
      mask: allMasks,
    });
  }

  async toggleTrackGroup(locator: Locator) {
    await locator.locator('.pf-track__shell').first().click();
    await this.waitForPerfettoIdle();
  }

  locateTrack(name: string, trackGroup?: Locator): Locator {
    return (trackGroup ?? this.page).locator(`.pf-track[ref="${name}"]`);
  }

  pinTrackUsingShellBtn(track: Locator) {
    track.locator('button[title="Pin to top"]').click({force: true});
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  async runCommand(cmdId: string, ...args: any[]) {
    await this.page.evaluate(
      (arg) => self.app.commands.runCommand(arg.cmdId, ...arg.args),
      {cmdId, args},
    );
  }

  async disableOmniboxPrompt() {
    await this.page.evaluate(() =>
      (self.app as AppImpl).omnibox.disablePrompts(),
    );
  }

  async searchSlice(name: string) {
    const omnibox = this.page.locator('input[ref=omnibox]');
    await omnibox.focus();
    await omnibox.fill(name);
    await this.waitForPerfettoIdle();
    await omnibox.press('Enter');
    await this.waitForPerfettoIdle();
  }

  getTestTracePath(fname: string): string {
    const parts = ['test', 'data', fname];
    if (process.cwd().endsWith('/ui')) {
      parts.unshift('..');
    }
    const fPath = path.join(...parts);
    if (!fs.existsSync(fPath)) {
      throw new Error(`Could not locate file ${fPath}, cwd=${process.cwd()}`);
    }
    return fPath;
  }

  async clickMenuItem(text: string | RegExp) {
    await this.page
      .locator('.pf-popup-content .pf-menu-item', {hasText: text})
      .click();
  }

  async switchToTab(text: string | RegExp) {
    await this.page.locator('.pf-split-panel__tab', {hasText: text}).click();
  }

  async scheduleFullRedraw(): Promise<void> {
    await this.page.evaluate(() => self.app.raf.scheduleFullRedraw());
  }
}
