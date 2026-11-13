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

import {defer} from '../base/deferred';
import type {Policy} from '../public/policy';
import {initializeAppImplForTesting} from './fake_trace_impl';

function policyWithAll(value: boolean): Policy {
  return {
    enableTraceSharing: value,
    enableErrorTraceUpload: value,
    useInternalBugLinks: value,
    enableSqlAgent: value,
    showHiringBanner: value,
  };
}

// AppImpl is a singleton that reads the saved answer on first access, so the
// tests below run in order and share state.
describe('AppImpl', () => {
  describe('policy', () => {
    test('should start from the answer saved on the previous visit', () => {
      localStorage.setItem('isInternalUser', '1');
      const app = initializeAppImplForTesting();
      expect(app.policy).toEqual(policyWithAll(true));
    });
  });

  describe('setIsInternalUser', () => {
    test("should keep the saved policy until this visit's answer arrives", async () => {
      const app = initializeAppImplForTesting();
      const answer = defer<boolean>();
      app.setIsInternalUser(answer);
      expect(app.policy).toEqual(policyWithAll(true));

      answer.resolve(false);
      await answer;
      expect(app.policy).toEqual(policyWithAll(false));
      expect(localStorage.getItem('isInternalUser')).toBe('0');
    });

    test('should turn every feature on for internal users', async () => {
      const app = initializeAppImplForTesting();
      const answer = Promise.resolve(true);
      app.setIsInternalUser(answer);
      await answer;
      expect(app.policy).toEqual(policyWithAll(true));
      expect(localStorage.getItem('isInternalUser')).toBe('1');
    });
  });
});
