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
import type {
  AdvancedAnalyticsEvent,
  TraceLoadId,
} from './analytics/analytics_impl';
import {initializeAppImplForTesting} from './fake_trace_impl';

const gtagGlobals = window as {} as {dataLayer: IArguments[]};

// The names of the gtag('event', ...) calls since dataLayer was last cleared.
function sentEventNames(): unknown[] {
  return gtagGlobals.dataLayer
    .map((entry) => Array.from(entry))
    .filter(([command]) => command === 'event')
    .map(([, name]) => name);
}

function policyFor(isInternalUser: boolean): Policy {
  return {
    enableTraceSharing: isInternalUser,
    enableTraceUploadForBugReports: isInternalUser,
    bugReportUrl: isInternalUser
      ? 'https://goto.google.com/perfetto-ui-bug'
      : 'https://github.com/google/perfetto/issues/new',
    useAdvancedAnalytics: isInternalUser,
  };
}

// AppImpl is a singleton that reads the saved answer on first access, so the
// tests below run in order and share state.
describe('AppImpl', () => {
  describe('policy', () => {
    test('should start from the answer saved on the previous visit', () => {
      localStorage.setItem('isInternalUser', '1');
      const app = initializeAppImplForTesting();
      expect(app.policy).toEqual(policyFor(true));
    });
  });

  describe('setIsInternalUser', () => {
    test("should keep the saved policy until this visit's answer arrives", async () => {
      const app = initializeAppImplForTesting();
      const answer = defer<boolean>();
      app.setIsInternalUser(answer);
      expect(app.policy).toEqual(policyFor(true));

      answer.resolve(false);
      await answer;
      expect(app.policy).toEqual(policyFor(false));
      expect(localStorage.getItem('isInternalUser')).toBe('0');
    });

    test('should turn every feature on for internal users', async () => {
      const app = initializeAppImplForTesting();
      const answer = Promise.resolve(true);
      app.setIsInternalUser(answer);
      await answer;
      expect(app.policy).toEqual(policyFor(true));
      expect(localStorage.getItem('isInternalUser')).toBe('1');
    });
  });

  describe('analytics', () => {
    test('should send advanced events only while the policy allows them', async () => {
      const app = initializeAppImplForTesting();
      // The test origin (localhost) picks an embedder with a GA ID, so this
      // is the real analytics class rather than the no-op one.
      expect(app.analytics.isEnabled()).toBe(true);
      const event: AdvancedAnalyticsEvent = {
        kind: 'plugin_loaded',
        traceLoadId: 'load-1' as TraceLoadId,
        plugin: 'dev.perfetto.Sched',
        loadMs: 312,
        hasTracks: true,
      };

      const external = Promise.resolve(false);
      app.setIsInternalUser(external);
      await external;
      gtagGlobals.dataLayer = [];
      app.analytics.logAdvancedEvent(event);
      expect(sentEventNames()).not.toContain('plugin_loaded');

      const internal = Promise.resolve(true);
      app.setIsInternalUser(internal);
      await internal;
      app.analytics.logAdvancedEvent(event);
      expect(sentEventNames()).toContain('plugin_loaded');
    });
  });
});
