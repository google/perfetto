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
import {assertExists} from '../base/assert';
import {Size2D} from '../base/geom';
import {AppImpl} from '../core/app_impl';

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
    await assertExists(file).setInputFiles(tracePath);
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
    await expect.soft(target).toHaveScreenshot(screenshotName, {
      ...screenshotOpts,
      mask: opts?.mask,
    });
  }

  async toggleTrackGroup(locator: Locator) {
    await locator.locator('.pf-track__shell').first().click();
    await this.scheduleFullRedraw();
    await this.waitForPerfettoIdle();
  }

  locateTrack(path: string | readonly string[]): Locator {
    const ref = Array.isArray(path) ? path.join('/') : path;
    return this.page.locator(`.pf-track[ref="${ref}"]`);
  }

  /**
   * Scrolls to a track by path, bringing it into view even with virtual
   * scrolling enabled. Returns a locator for the track.
   *
   * @param path - A slash-separated path string (e.g. 'GPU/GPU Frequency')
   *
   * Use this instead of locateTrack() when the track might be outside the
   * viewport (and thus not rendered in the DOM with virtual scrolling).
   */
  async scrollToTrack(path: string): Promise<Locator> {
    const pathArray = path.split('/');
    await this.page.evaluate((targetPath) => {
      const trace = (self.app as AppImpl).trace;
      if (!trace) return;
      const node = trace.defaultWorkspace.flatTracks.find((t) => {
        const fullPath = t.fullPath;
        if (t.headless) return false;
        if (fullPath.length !== targetPath.length) return false;
        return fullPath.every((segment, i) => segment === targetPath[i]);
      });
      if (node) {
        trace.tracks.scrollToTrackNodeId = node.id;
        self.app.raf.scheduleFullRedraw();
      } else {
        throw new Error(
          `Could not find track with path ${targetPath.join('/')}`,
        );
      }
    }, pathArray);
    await this.waitForPerfettoIdle();
    return this.locateTrack(path);
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
    await this.page.locator('.pf-drawer-panel__tab', {hasText: text}).click();
  }

  async scheduleFullRedraw(): Promise<void> {
    await this.page.evaluate(() => self.app.raf.scheduleFullRedraw());
  }
}
