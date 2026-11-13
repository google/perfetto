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

// These tests live apart from app_impl_unittest.ts because AppImpl is a
// singleton per test file and analytics.initialize() can run only once, so
// they need an AppImpl that no other test has passed an answer to.

import {initializeAppImplForTesting} from './fake_trace_impl';

const gtagGlobals = window as {} as {dataLayer: IArguments[]};

// Returns the fields passed to gtag('config', ...), or undefined if GA hasn't
// been configured yet.
function gaConfig(): Record<string, unknown> | undefined {
  for (const entry of gtagGlobals.dataLayer) {
    const [command, , fields] = Array.from(entry);
    if (command === 'config') return fields as Record<string, unknown>;
  }
  return undefined;
}

describe('AppImpl', () => {
  describe('setIsInternalUser', () => {
    test('should send perfetto_is_internal_user to GA', async () => {
      const app = initializeAppImplForTesting();
      // The test origin (localhost) picks an embedder with a GA ID, so this
      // is the real analytics class rather than the no-op one.
      expect(app.analytics.isEnabled()).toBe(true);

      app.setIsInternalUser(Promise.resolve(true));
      await app.analytics.initialize();
      expect(gaConfig()?.perfetto_is_internal_user).toBe('1');
    });
  });
});
