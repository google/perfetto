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

// A Playwright reporter that collects every failing toHaveScreenshot()
// comparison into a single static HTML page, so that all the diffs can be
// reviewed at a glance rather than clicking through each test in the default
// HTML report.
//
// The page itself lives in screenshot_diff_viewer.html. This reporter only
// gathers the data, copies the images and injects the data as JSON.
//
// Must be listed *after* the 'html' reporter in playwright.config.ts when
// writing inside the html report folder, because the html reporter wipes its
// output folder in onEnd() and reporters' onEnd() run sequentially in order.

import * as fs from 'fs';
import * as path from 'path';
import type {Reporter, TestCase, TestResult} from '@playwright/test/reporter';

interface ScreenshotDiffReporterOptions {
  // Directory where index.html and the copied images are written.
  outputFolder: string;
  // Output folder of the 'html' reporter, if any. Used to link each failure to
  // its page in the native Playwright report.
  htmlReportFolder?: string;
}

type Role = 'expected' | 'actual' | 'diff';

// Must be kept in sync with the consumer in screenshot_diff_viewer.html.
interface ScreenshotFailure {
  testTitle: string;
  screenshotName: string;
  // Link to this test's attempt in the native Playwright HTML report.
  reportUrl?: string;
  // Role -> image path relative to the output folder.
  images: Partial<Record<Role, string>>;
}

interface ViewerData {
  ciJobId?: string;
  reportUrl?: string;
  failures: ScreenshotFailure[];
}

const VIEWER_TEMPLATE = path.join(__dirname, 'screenshot_diff_viewer.html');
const DATA_PLACEHOLDER = '"__SCREENSHOT_DIFF_DATA__"';

// toHaveScreenshot() attaches images named '<name>-{expected,actual,diff}.png'.
const ATTACHMENT_RE = /^(.*)-(expected|actual|diff)\.png$/;

export default class ScreenshotDiffReporter implements Reporter {
  private readonly outputFolder: string;
  private readonly htmlReportRelPath?: string;
  // Latest result for each test. With retries, onTestEnd() fires once per
  // attempt; only the last attempt matters.
  private readonly lastResults = new Map<TestCase, TestResult>();
  private nextId = 0;

  constructor(options: ScreenshotDiffReporterOptions) {
    this.outputFolder = path.resolve(options.outputFolder);
    if (options.htmlReportFolder !== undefined) {
      const rel = path.relative(
        this.outputFolder,
        path.resolve(options.htmlReportFolder, 'index.html'),
      );
      this.htmlReportRelPath = rel.split(path.sep).join('/');
    }
  }

  printsToStdio(): boolean {
    return false;
  }

  onTestEnd(test: TestCase, result: TestResult): void {
    this.lastResults.set(test, result);
  }

  onEnd(): void {
    fs.rmSync(this.outputFolder, {recursive: true, force: true});
    fs.mkdirSync(this.outputFolder, {recursive: true});

    const failures: ScreenshotFailure[] = [];
    for (const [test, result] of this.lastResults) {
      // Skip passing and flaky (failed, then passed on retry) tests.
      if (test.outcome() !== 'unexpected') continue;
      failures.push(...this.collectFailures(test, result));
    }

    const data: ViewerData = {
      ciJobId: process.env.PERFETTO_CI_JOB_ID,
      reportUrl: this.htmlReportRelPath,
      failures,
    };
    // Escape '<' so the JSON can't close the surrounding <script> tag.
    const json = JSON.stringify(data).replace(/</g, '\\u003c');
    const html = fs
      .readFileSync(VIEWER_TEMPLATE, 'utf8')
      .replace(DATA_PLACEHOLDER, () => json);
    fs.writeFileSync(path.join(this.outputFolder, 'index.html'), html);
  }

  private collectFailures(
    test: TestCase,
    result: TestResult,
  ): ScreenshotFailure[] {
    // titlePath() is ['', project, file, ...describes, title]. Show the file's
    // basename, which is also the {testFileName} used for snapshot folders.
    const testTitle = [
      path.basename(test.location.file),
      ...test.titlePath().slice(3),
    ].join(' › ');
    const reportUrl =
      this.htmlReportRelPath === undefined
        ? undefined
        : `${this.htmlReportRelPath}#?testId=${encodeURIComponent(test.id)}` +
          `&run=${result.retry}`;
    const byName = new Map<string, ScreenshotFailure & {id: number}>();
    for (const a of result.attachments) {
      const m = ATTACHMENT_RE.exec(a.name);
      if (m === null || a.path === undefined || !fs.existsSync(a.path)) {
        continue;
      }
      const screenshotName = m[1];
      const role = m[2] as Role;
      let failure = byName.get(screenshotName);
      if (failure === undefined) {
        failure = {
          testTitle,
          screenshotName,
          reportUrl,
          images: {},
          id: this.nextId++,
        };
        byName.set(screenshotName, failure);
      }
      const fileName = `${failure.id}-${role}.png`;
      fs.copyFileSync(a.path, path.join(this.outputFolder, fileName));
      failure.images[role] = fileName;
    }
    // Only keep entries that have an actual image: that's what a failed
    // comparison (or a missing baseline) produces.
    return [...byName.values()]
      .filter((f) => f.images.actual !== undefined)
      .map(({id: _id, ...f}) => f);
  }
}
