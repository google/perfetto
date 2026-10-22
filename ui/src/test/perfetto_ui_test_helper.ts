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
  test,
  type Locator,
  type Page,
  type PageAssertionsToHaveScreenshotOptions,
} from '@playwright/test';
import crypto from 'crypto';
import fs from 'fs';
import path from 'path';
import type {IdleDetectorWindow} from '../frontend/idle_detector_interface';
import {ensureExists} from '../base/assert';
import type {Size2D} from '../base/geom';
import type {AppImpl} from '../core/app_impl';

// Mirrors sanitizeForFilePath() in Playwright's fileUtils.js.
function sanitizeForFilePath(s: string): string {
  return s.replace(/[\x00-\x2C\x2E-\x2F\x3A-\x40\x5B-\x60\x7B-\x7F]+/g, '-');
}

// Mirrors trimLongString() in Playwright's util.js.
function trimLongString(s: string, length = 100): string {
  if (s.length <= length) return s;
  const hash = crypto.createHash('sha1').update(s).digest('hex');
  const middle = `-${hash.substring(0, 5)}-`;
  const start = Math.floor((length - middle.length) / 2);
  const end = length - middle.length - start;
  return s.substring(0, start) + middle + s.slice(-end);
}

export class PerfettoTestHelper {
  private cachedSidebarSize?: Size2D;
  private currentStepTitle?: string;

  constructor(readonly page: Page) {}

  // Runs `body` as a Playwright step. Screenshots taken inside the step are
  // stored under a folder named after the step title instead of the test
  // title. This lets one test contain several steps, each with its own
  // screenshot folder. A screenshot mismatch is a soft failure, so it doesn't
  // stop the following steps from running.
  async step(title: string, body: () => Promise<void>): Promise<void> {
    await test.step(title, async () => {
      this.currentStepTitle = title;
      try {
        await body();
      } finally {
        this.currentStepTitle = undefined;
      }
    });
  }

  // Returns the name to pass to toHaveScreenshot(). Combined with
  // snapshotPathTemplate in playwright.config.ts, screenshots are stored at
  // <file>/<test or step title>/<name>. Playwright doesn't sanitize array
  // names, so this sanitizes the folder the same way Playwright sanitizes
  // {testName}. Every screenshot must use this, otherwise it ends up directly
  // under <file>/.
  screenshotName(name: string): string[] {
    const titlePath = test.info().titlePath.slice(1);
    if (this.currentStepTitle !== undefined) {
      titlePath[titlePath.length - 1] = this.currentStepTitle;
    }
    const folder = sanitizeForFilePath(trimLongString(titlePath.join(' ')));
    const ext = path.extname(name);
    const base = name.substring(0, name.length - ext.length);
    return [folder, sanitizeForFilePath(base) + ext];
  }

  resetFocus(): Promise<void> {
    return this.page.click('.pf-sidebar img.pf-sidebar__brand');
  }

  async sidebarSize(): Promise<Size2D> {
    if (this.cachedSidebarSize === undefined) {
      const size = await this.page.locator('main > .pf-sidebar').boundingBox();
      this.cachedSidebarSize = ensureExists(size);
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
    await ensureExists(file).setInputFiles(tracePath);
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
        .pf-test-volatile {
          visibility: hidden !important;
        }
      `,
    });
  }

  async waitForPerfettoIdle(idleHysteresisMs?: number): Promise<void> {
    await this.page.waitForFunction(
      () =>
        typeof (window as {} as {waitForPerfettoIdle?: unknown})
          .waitForPerfettoIdle === 'function',
    );
    return this.page.evaluate(
      async (ms) =>
        (window as {} as IdleDetectorWindow).waitForPerfettoIdle(ms),
      idleHysteresisMs,
    );
  }

  async waitForIdleAndScreenshot(
    screenshotName: string,
    opts?: PageAssertionsToHaveScreenshotOptions & {locator?: Locator},
  ) {
    await this.page.mouse.move(0, 0); // Move mouse out of the way.
    await this.waitForPerfettoIdle();

    const {locator, ...screenshotOpts} = opts ?? {};
    const target = locator ?? this.page;

    // Call the original expect with the combined masks.
    await expect
      .soft(target)
      .toHaveScreenshot(this.screenshotName(screenshotName), {
        ...screenshotOpts,
        mask: opts?.mask,
      });
  }

  async toggleTrackGroup(locator: Locator) {
    await locator.locator('.pf-track__shell').first().click();
    await this.waitForPerfettoIdle();
  }

  async expandTrackGroup(locator: Locator) {
    const header = locator.locator(':scope > .pf-track__header');
    const classes = await header.getAttribute('class');
    if (!classes?.includes('pf-track__header--expanded')) {
      await this.toggleTrackGroup(locator);
    }
  }

  locateTrack(name: string, trackGroup?: Locator): Locator {
    return (trackGroup ?? this.page).locator(`.pf-track[ref="${name}"]`);
  }

  async pinTrackUsingShellBtn(track: Locator) {
    await track.locator('.pf-track__shell').hover();
    await track.locator('button[title="Pin to top"]').click();
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
    await this.page
      .locator('.pf-drawer-panel .pf-tab-strip__tab', {hasText: text})
      .click();
  }

  async scheduleFullRedraw(): Promise<void> {
    await this.page.evaluate(() => self.app.raf.scheduleFullRedraw());
  }
}
