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

import type {AdvancedAnalyticsEvent, AnalyticsInternal} from './analytics_impl';
import {createTraceLoadId} from './analytics_impl';
import type {BasicAnalyticsEvent} from '../../public/analytics';
import type {ErrorDetails} from '../../base/logging';
import {PluginLoadReporter} from './plugin_load_reporter';

// Analytics that collects the advanced events it is given.
class FakeAnalytics implements AnalyticsInternal {
  readonly sent: AdvancedAnalyticsEvent[] = [];
  logAdvancedEvent(event: AdvancedAnalyticsEvent) {
    this.sent.push(event);
  }
  addDimension(
    _arg: {key: string; value: string} | Promise<{key: string; value: string}>,
  ) {}
  setIsInternalUser(_isInternalUser: Promise<boolean>) {}
  async initialize() {}
  logEvent(_event: BasicAnalyticsEvent) {}
  logError(_err: ErrorDetails) {}
  isEnabled(): boolean {
    return true;
  }
}

// A reporter that collects the events it sends.
function createReporter() {
  const analytics = new FakeAnalytics();
  const traceLoadId = createTraceLoadId();
  const reporter = new PluginLoadReporter(traceLoadId, analytics);
  return {reporter, sent: analytics.sent, traceLoadId};
}

// The plugins named in the sent events, in order.
function sentPlugins(sent: AdvancedAnalyticsEvent[]): string[] {
  return sent.map((e) => (e.kind === 'plugin_loaded' ? e.plugin : e.kind));
}

// Load times for |count| fast plugins named 'fast0', 'fast1', ..., each taking
// 1 ms.
function fastPlugins(count: number): Array<[string, number]> {
  return Array.from({length: count}, (_, i) => [`fast${i}`, 1]);
}

describe('PluginLoadReporter', () => {
  describe('reportLoad', () => {
    test('should report every plugin that registered tracks', () => {
      const {reporter, sent} = createReporter();
      // 'quick' is among the fastest, but registered tracks.
      const loadTimes = new Map([...fastPlugins(20), ['quick', 0]]);
      reporter.reportLoad(loadTimes, new Set(['quick']));
      expect(sentPlugins(sent)).toContain('quick');
    });

    test('should report the 10 slowest plugins, even without tracks', () => {
      const {reporter, sent} = createReporter();
      const slow: Array<[string, number]> = Array.from({length: 10}, (_, i) => [
        `slow${i}`,
        100 + i,
      ]);
      reporter.reportLoad(new Map([...fastPlugins(5), ...slow]), new Set());
      expect(sentPlugins(sent).sort()).toEqual(slow.map(([id]) => id).sort());
    });

    test('should report a plugin in both groups once', () => {
      const {reporter, sent} = createReporter();
      reporter.reportLoad(new Map([['sched', 300]]), new Set(['sched']));
      expect(sentPlugins(sent)).toEqual(['sched']);
    });

    test('should send the trace load ID, load time and tracks flag', () => {
      const {reporter, sent, traceLoadId} = createReporter();
      reporter.reportLoad(
        new Map([
          ['sched', 312.4],
          ['empty', 50],
        ]),
        new Set(['sched']),
      );
      expect(sent).toEqual([
        {
          kind: 'plugin_loaded',
          traceLoadId,
          plugin: 'sched',
          loadMs: 312,
          hasTracks: true,
        },
        {
          kind: 'plugin_loaded',
          traceLoadId,
          plugin: 'empty',
          loadMs: 50,
          hasTracks: false,
        },
      ]);
    });
  });
});
