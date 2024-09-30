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

export class PerfettoTestHelper {
  private cachedSidebarSize?: Size2D;

  constructor(readonly page: Page) {}

  resetFocus(): Promise<void> {
    return this.page.click('.sidebar img.brand');
  }

  async sidebarSize(): Promise<Size2D> {
    if (this.cachedSidebarSize === undefined) {
      const size = await this.page.locator('main > .sidebar').boundingBox();
      this.cachedSidebarSize = assertExists(size);
    }
    return this.cachedSidebarSize;
  }

  async navigate(fragment: string): Promise<void> {
    await this.page.goto('/?testing=1' + fragment);
    await this.waitForPerfettoIdle();
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
    await this.page.mouse.move(0, 0);
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
    await expect(this.page).toHaveScreenshot(screenshotName, opts);
  }

  locateTrackGroup(name: string): Locator {
    return this.page
      .locator('.pf-panel-group')
      .filter({has: this.page.locator(`h1[ref="${name}"]`)});
  }

  async toggleTrackGroup(locator: Locator) {
    await locator.locator('.pf-track-title').first().click();
    await this.waitForPerfettoIdle();
  }

  locateTrack(name: string, trackGroup?: Locator): Locator {
    return (trackGroup ?? this.page)
      .locator('.pf-track')
      .filter({has: this.page.locator(`h1[ref="${name}"]`)});
  }

  pinTrackUsingShellBtn(track: Locator) {
    track.locator('button[title="Pin to top"]').click({force: true});
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  async runCommand(cmdId: string, ...args: any[]) {
    await this.page.evaluate(
      (arg) => self.globals.commandManager.runCommand(arg.cmdId, ...arg.args),
      {cmdId, args},
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
}
