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

import {defer} from '../../base/deferred';
import {initAnalytics} from './analytics_impl';

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

// Returns the [name, fields] of every gtag('event', ...) call so far.
function gaEvents(): Array<[unknown, unknown]> {
  return gtagGlobals.dataLayer
    .map((entry) => Array.from(entry))
    .filter(([command]) => command === 'event')
    .map(([, name, fields]) => [name, fields]);
}

describe('AnalyticsImpl', () => {
  describe('logEvent', () => {
    beforeEach(() => {
      gtagGlobals.dataLayer = [];
    });

    test('should send a page view as an event named after the page', () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      analytics.logEvent({kind: 'page_view', page: '/viewer'});
      expect(gaEvents()).toContainEqual([
        '/viewer',
        {event_category: 'User Actions'},
      ]);
    });

    test('should send an action with its category', () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      analytics.logEvent({
        kind: 'action',
        category: 'Trace Actions',
        action: 'Download trace',
      });
      expect(gaEvents()).toContainEqual([
        'Download trace',
        {event_category: 'Trace Actions'},
      ]);
    });

    test('should send an action without a category', () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      analytics.logEvent({kind: 'action', action: 'MyPlugin:Activated'});
      expect(gaEvents()).toContainEqual([
        'MyPlugin:Activated',
        {event_category: null},
      ]);
    });
  });

  describe('setIsInternalUser', () => {
    beforeEach(() => {
      gtagGlobals.dataLayer = [];
    });

    test("should set perfetto_is_internal_user to '1' for internal users", async () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      analytics.setIsInternalUser(Promise.resolve(true));
      await analytics.initialize();
      expect(gaConfig()?.perfetto_is_internal_user).toBe('1');
    });

    test("should set perfetto_is_internal_user to '0' for external users", async () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      analytics.setIsInternalUser(Promise.resolve(false));
      await analytics.initialize();
      expect(gaConfig()?.perfetto_is_internal_user).toBe('0');
    });

    test('should not start GA until the answer arrives', async () => {
      const analytics = initAnalytics(false, false, true, 'G-TEST');
      const answer = defer<boolean>();
      analytics.setIsInternalUser(answer);
      const initialized = analytics.initialize();
      await Promise.resolve();
      expect(gaConfig()).toBeUndefined();

      answer.resolve(true);
      await initialized;
      expect(gaConfig()?.perfetto_is_internal_user).toBe('1');
    });
  });
});
