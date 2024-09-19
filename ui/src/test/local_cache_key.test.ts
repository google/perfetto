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

import {test, expect} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test('multiple traces via url and local_cache_key', async ({browser}) => {
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);

  // Open first trace.
  await pth.navigate(
    '#!/?url=http://127.0.0.1:10000/test/data/perf_sample_annotations.pftrace',
  );
  const cacheKey1 = page.url().match(/local_cache_key=([a-z0-9-]+)/)![1];
  await expect(page).toHaveScreenshot('trace_1.png');

  // Open second trace.
  await pth.navigate(
    '#!/?url=http://127.0.0.1:10000/test/data/atrace_compressed.ctrace',
  );
  const cacheKey2 = page.url().match(/local_cache_key=([a-z0-9-]+)/)![1];
  expect(cacheKey1).not.toEqual(cacheKey2);
  await expect(page).toHaveScreenshot('trace_2.png');

  // Navigate back to the first trace. A confirmation dialog will be shown
  await pth.navigate('#!/viewer?local_cache_key=' + cacheKey1);
  await expect(page).toHaveScreenshot('confirmation_dialog.png');

  await page.locator('button.modal-btn-primary').click();
  await pth.waitForPerfettoIdle();
  await expect(page).toHaveScreenshot('back_to_trace_1.png');
});
