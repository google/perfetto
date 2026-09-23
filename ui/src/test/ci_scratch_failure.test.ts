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
 * DO NOT SUBMIT. Scratch test used to exercise the "Comment PR with UI test
 * report link" step in .github/workflows/ui-tests.yml.
 *
 * This now passes, to confirm that a green Playwright run adds no test entry
 * to the PR comment.
 *
 * NOTE: this file must not simply be deleted to end the experiment. If the
 * diff contains only .github/ paths, analyze.yml sets TRIVIAL_CHANGE=1 and
 * skips the ui job altogether, so no comment would be posted at all.
 */
test('scratch test for the CI report link', async () => {
  expect(1).toBe(1);
});
