// Copyright (C) 2024 The Android Open Source Project
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

import {defineConfig} from '@playwright/test';
import * as os from 'os';

const isMac = os.platform() === 'darwin';
const isCi = Boolean(process.env.CI);
const outDir = process.env.OUT_DIR ?? '../out/ui';

export default defineConfig({
  timeout: 30_000,
  testDir: './src',
  snapshotDir: '../test/data/ui-screenshots',
  snapshotPathTemplate: '{snapshotDir}/{testFileName}/{testName}/{arg}{ext}',
  outputDir: `${outDir}/ui-test-results`,
  fullyParallel: true,
  retries: isCi ? 2 : 0, // Retry only in CI
  reporter: [
    [
      'html',
      {
        outputFolder: `${outDir}/ui-test-artifacts`,
        open: isCi ? 'never' : 'on-failure',
      },
    ],
  ],

  expect: {
    timeout: 5000,
    toHaveScreenshot: {
      // Rendering is not 100% identical on Mac. Be more tolerant.
      // Otherwise, allow for small differences between rasterizers on different platforms.
      maxDiffPixelRatio: isMac ? 0.05 : undefined,
    },
  },

  use: {
    baseURL: 'http://127.0.0.1:10000',
    trace: 'off',
  },

  projects: [
    {
      name: 'chromium',
      use: {
        headless: true,
        viewport: {width: 1920, height: 1080},
        launchOptions: {
          args: [
            '--headless',
            '--disable-accelerated-2d-canvas',
            '--disable-font-subpixel-positioning',
            '--ignore-gpu-blocklist', // Allow llvmpipe software rendering
            '--use-angle=gl',
            '--disable-lcd-text',
            '--disable-spell-checking',
            '--font-render-hinting=none',
            '--force-device-scale-factor=1',
            '--hide-scrollbars',
            '--enable-skia-renderer',
            '--js-flags=--random-seed=1',
          ],
        },
        ignoreHTTPSErrors: true,
        trace: 'off',
        screenshot: 'on',
        video: 'off',
      },
    },
  ],

  webServer: {
    // Just run the server without building
    command:
      './run-dev-server --no-build --no-depscheck ' +
      (process.env.DEV_SERVER_ARGS ?? ''),
    url: 'http://127.0.0.1:10000',
    reuseExistingServer: true,
    timeout: 5 * 60 * 1000,
    stdout: 'pipe',
  },
});
