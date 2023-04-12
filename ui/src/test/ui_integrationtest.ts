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

import fs from 'fs';
import path from 'path';
import {Browser, Page} from 'puppeteer';

import {assertExists} from '../base/logging';

import {
  compareScreenshots,
  failIfTraceProcessorHttpdIsActive,
  getTestTracePath,
  waitForPerfettoIdle,
} from './perfetto_ui_test_helper';

declare let global: {__BROWSER__: Browser;};
const browser = assertExists(global.__BROWSER__);
const expectedScreenshotPath = path.join('test', 'data', 'ui-screenshots');
const tmpDir = path.resolve('./ui-test-artifacts');
const reportPath = path.join(tmpDir, 'report.txt');

async function getPage(): Promise<Page> {
  const pages = (await browser.pages());
  expect(pages.length).toBe(1);
  return pages[pages.length - 1];
}

// Executed once at the beginning of the test. Navigates to the UI.
beforeAll(async () => {
  await failIfTraceProcessorHttpdIsActive();
  jest.setTimeout(60000);
  const page = await getPage();
  await page.setViewport({width: 1920, height: 1080});

  // Empty the file with collected screenshot diffs
  fs.writeFileSync(reportPath, '');
});

// After each test (regardless of nesting) capture a screenshot named after the
// test('') name and compare the screenshot with the expected one in
// /test/data/ui-screenshots.
afterEach(async () => {
  let testName = expect.getState().currentTestName;
  testName = testName.replace(/[^a-z0-9-]/gmi, '_').toLowerCase();
  const page = await getPage();

  const screenshotName = `ui-${testName}.png`;
  const actualFilename = path.join(tmpDir, screenshotName);
  const expectedFilename = path.join(expectedScreenshotPath, screenshotName);
  await page.screenshot({path: actualFilename});
  const rebaseline = process.env['PERFETTO_UI_TESTS_REBASELINE'] === '1';
  if (rebaseline) {
    console.log('Saving reference screenshot into', expectedFilename);
    fs.copyFileSync(actualFilename, expectedFilename);
  } else {
    await compareScreenshots(reportPath, actualFilename, expectedFilename);
  }
});

describe('android_trace_30s', () => {
  let page: Page;

  beforeAll(async () => {
    page = await getPage();
    await page.goto('http://localhost:10000/?testing=1');
    await waitForPerfettoIdle(page);
  });

  test('load', async () => {
    const file = await page.waitForSelector('input.trace_file');
    const tracePath = getTestTracePath('example_android_trace_30s.pb');
    assertExists(file).uploadFile(tracePath);
    await waitForPerfettoIdle(page);
  });

  test('expand_camera', async () => {
    await page.click('.main-canvas');
    await page.click('h1[title="com.google.android.GoogleCamera 5506"]');
    await page.evaluate(() => {
      document.querySelector('.scrolling-panel-container')!.scrollTo(0, 400);
    });
    await waitForPerfettoIdle(page);
  });
});

describe('chrome_rendering_desktop', () => {
  let page: Page;

  beforeAll(async () => {
    page = await getPage();
    await page.goto('http://localhost:10000/?testing=1');
    await waitForPerfettoIdle(page);
  });

  test('load', async () => {
    const page = await getPage();
    const file = await page.waitForSelector('input.trace_file');
    const tracePath = getTestTracePath('chrome_rendering_desktop.pftrace');
    assertExists(file).uploadFile(tracePath);
    await waitForPerfettoIdle(page);
  });

  test('expand_browser_proc', async () => {
    const page = await getPage();
    await page.click('.main-canvas');
    await page.click('h1[title="Browser 12685"]');
    await waitForPerfettoIdle(page);
  });

  test('select_slice_with_flows', async () => {
    const page = await getPage();
    const searchInput = '.omnibox input';
    await page.focus(searchInput);
    await page.keyboard.type('GenerateRenderPass');
    await waitForPerfettoIdle(page);
    for (let i = 0; i < 3; i++) {
      await page.keyboard.type('\n');
    }
    await waitForPerfettoIdle(page);
    await page.focus('canvas');
    await page.keyboard.type('f');  // Zoom to selection
    await waitForPerfettoIdle(page);
  });
});

// Tests that chrome traces with missing process/thread names still open
// correctly in the UI.
describe('chrome_missing_track_names', () => {
  let page: Page;

  beforeAll(async () => {
    page = await getPage();
    await page.goto('http://localhost:10000/?testing=1');
    await waitForPerfettoIdle(page);
  });

  test('load', async () => {
    const page = await getPage();
    const file = await page.waitForSelector('input.trace_file');
    const tracePath = getTestTracePath('chrome_missing_track_names.pb.gz');
    assertExists(file).uploadFile(tracePath);
    await waitForPerfettoIdle(page);
  });
});

describe('routing', () => {
  describe('open_two_traces_then_go_back', () => {
    let page: Page;

    beforeAll(async () => {
      page = await getPage();
      await page.goto('http://localhost:10000/?testing=1');
      await waitForPerfettoIdle(page);
    });

    test('open_first_trace_from_url', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1/#!/?url=http://localhost:10000/test/data/chrome_memory_snapshot.pftrace');
      await waitForPerfettoIdle(page);
    });

    test('open_second_trace_from_url', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1#!/?url=http://localhost:10000/test/data/chrome_scroll_without_vsync.pftrace');
      await waitForPerfettoIdle(page);
    });

    test('access_subpage_then_go_back', async () => {
      await waitForPerfettoIdle(page);
      await page.goto(
          'http://localhost:10000/?testing=1/#!/metrics?local_cache_key=76c25a80-25dd-1eb7-2246-d7b3c7a10f91');
      await page.goBack();
      await waitForPerfettoIdle(page);
    });
  });

  describe('start_from_no_trace', () => {
    let page: Page;

    beforeAll(async () => {
      page = await getPage();
      await page.goto('about:blank');
    });

    test('go_to_page_with_no_trace', async () => {
      await page.goto('http://localhost:10000/?testing=1#!/info');
      await waitForPerfettoIdle(page);
    });

    test('open_trace ', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1#!/viewer?local_cache_key=76c25a80-25dd-1eb7-2246-d7b3c7a10f91');
      await waitForPerfettoIdle(page);
    });

    test('refresh', async () => {
      await page.reload();
      await waitForPerfettoIdle(page);
    });

    test('open_second_trace', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1#!/viewer?local_cache_key=00000000-0000-0000-e13c-bd7db4ff646f');
      await waitForPerfettoIdle(page);

      // click on the 'Continue' button in the interstitial
      await page.click('[id="trace_id_open"]');
      await waitForPerfettoIdle(page);
    });

    test('go_back_to_first_trace', async () => {
      await page.goBack();
      await waitForPerfettoIdle(page);
      // click on the 'Continue' button in the interstitial
      await page.click('[id="trace_id_open"]');
      await waitForPerfettoIdle(page);
    });

    test('open_invalid_trace', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1#!/viewer?local_cache_key=invalid');
      await waitForPerfettoIdle(page);
    });
  });

  describe('navigate', () => {
    let page: Page;

    beforeAll(async () => {
      page = await getPage();
      await page.goto('http://localhost:10000/?testing=1');
      await waitForPerfettoIdle(page);
    });

    test('open_trace_from_url', async () => {
      await page.goto(
          'http://localhost:10000/?testing=1/#!/?url=http://localhost:10000/test/data/chrome_memory_snapshot.pftrace');
      await waitForPerfettoIdle(page);
    });

    test('navigate_back_and_forward', async () => {
      await page.click('[id="info_and_stats"]');
      await waitForPerfettoIdle(page);
      await page.click('[id="metrics"]');
      await waitForPerfettoIdle(page);
      await page.goBack();
      await waitForPerfettoIdle(page);
      await page.goBack();
      await waitForPerfettoIdle(page);
      await page.goForward();
      await waitForPerfettoIdle(page);
      await page.goForward();
      await waitForPerfettoIdle(page);
    });
  });

  test('open_trace_and_go_back_to_landing_page', async () => {
    const page = await getPage();
    await page.goto('http://localhost:10000/?testing=1');
    await page.goto(
        'http://localhost:10000/?testing=1#!/viewer?local_cache_key=76c25a80-25dd-1eb7-2246-d7b3c7a10f91');
    await waitForPerfettoIdle(page);
    await page.goBack();
    await waitForPerfettoIdle(page);
  });

  test('open_invalid_trace_from_blank_page', async () => {
    const page = await getPage();
    await page.goto('about:blank');
    await page.goto(
        'http://localhost:10000/?testing=1#!/viewer?local_cache_key=invalid');
    await waitForPerfettoIdle(page);
  });
});

// Regression test for b/235335853.
describe('modal_dialog', () => {
  let page: Page;

  beforeAll(async () => {
    page = await getPage();
    await page.goto('http://localhost:10000/?testing=1');
    await waitForPerfettoIdle(page);
  });

  test('show_dialog_1', async () => {
    await page.click('#keyboard_shortcuts');
    await waitForPerfettoIdle(page);
  });

  test('dismiss_1', async () => {
    await page.keyboard.press('Escape');
    await waitForPerfettoIdle(page);
  });

  test('switch_page_no_dialog', async () => {
    await page.click('#record_new_trace');
    await waitForPerfettoIdle(page);
  });

  test('show_dialog_2', async () => {
    await page.click('#keyboard_shortcuts');
    await waitForPerfettoIdle(page);
  });

  test('dismiss_2', async () => {
    await page.keyboard.press('Escape');
    await waitForPerfettoIdle(page);
  });
});
