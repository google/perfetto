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

import {test, expect} from '@playwright/test';

/**
 * DO NOT SUBMIT. Scratch test that always fails, used to exercise the
 * "Comment PR with UI test report link" step in .github/workflows/ui-tests.yml.
 *
 * It fails via an assertion rather than a crash, so that Playwright still
 * writes out its HTML report (which is the artifact the comment links to).
 * It does not load a trace or the Perfetto UI, so it is fast even with the
 * two retries that CI applies.
 */
test('deliberate failure to exercise the CI report link', async () => {
  expect(1).toBe(2);
});
