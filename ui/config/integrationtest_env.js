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

const NodeEnvironment = require('jest-environment-node');
const puppeteer = require('puppeteer');

module.exports = class IntegrationtestEnvironment extends NodeEnvironment {
  constructor(config) {
    super(config);
  }

  async setup() {
    await super.setup();
    const headless = process.env.PERFETTO_UI_TESTS_INTERACTIVE !== '1';
    if (headless) {
      console.log('Starting Perfetto UI tests in headless mode.');
      console.log(
          'Pass --interactive to run-integrationtests or set ' +
          'PERFETTO_UI_TESTS_INTERACTIVE=1 to inspect the behavior ' +
          'in a visible Chrome window');
    }
    this.global.__BROWSER__ = await puppeteer.launch({
      args: [
        '--window-size=1920,1080',
        '--disable-accelerated-2d-canvas',
        '--disable-gpu',
        '--no-sandbox',  // Disable sandbox to run in Docker.
        '--disable-setuid-sandbox',
        '--font-render-hinting=none',
        '--enable-benchmarking',  // Disable finch and other sources of non
                                  // determinism.
      ],

      // This is so screenshot in --interactive and headless mode match. The
      // scrollbars are never part of the screenshot, but without this cmdline
      // switch, in headless mode we don't get any blank space (as if it was
      // overflow:hidden) and that changes the layout of the page.
      ignoreDefaultArgs: ['--hide-scrollbars'],

      headless: headless,
    });
  }

  async teardown() {
    if (this.global.__BROWSER__) {
      await this.global.__BROWSER__.close();
    }
    await super.teardown();
  }

  runScript(script) {
    return super.runScript(script);
  }
};
