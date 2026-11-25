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
import crypto from 'crypto';
import path from 'path';

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

// Returns the [folder, filename] tuple to pass to toHaveScreenshot(). Combined
// with snapshotPathTemplate in playwright.config.ts, screenshots are stored at
// <file>/<test or step title>/<name>. Playwright doesn't sanitize array names,
// so this sanitizes the folder the same way Playwright sanitizes {testName}.
export function formatScreenshotName(
  name: string,
  stepTitle?: string,
): string[] {
  const titlePath = test.info().titlePath.slice(1);
  if (stepTitle !== undefined) {
    titlePath[titlePath.length - 1] = stepTitle;
  }
  const folder = sanitizeForFilePath(trimLongString(titlePath.join(' ')));
  const ext = path.extname(name);
  const base = name.substring(0, name.length - ext.length);
  return [folder, sanitizeForFilePath(base) + ext];
}
