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

import m from 'mithril';
import type {App} from '../../../public/app';
import type {LiveSession, SnapshotData} from '../sessions/live_session';
import {ProcessesTab} from './processes_table';

describe('ProcessesTab live strip chart', () => {
  let root: HTMLElement;

  beforeEach(() => {
    root = document.createElement('div');
    document.body.appendChild(root);
  });

  afterEach(() => {
    m.mount(root, null);
    root.remove();
  });

  test('renders LiveStripChart in live mode and supports click-to-drill-down', () => {
    const ts1 = 10_000_000_000;
    const ts2 = 11_000_000_000;
    const MIB = 1024 * 1024;
    const data: SnapshotData = {
      systemCounters: new Map(),
      processCountersByUpid: new Map([
        [
          1,
          new Map([
            [
              'mem.rss',
              [
                {ts: ts1, value: 200 * MIB},
                {ts: ts2, value: 220 * MIB},
              ],
            ],
            [
              'mem.rss.anon',
              [
                {ts: ts1, value: 150 * MIB},
                {ts: ts2, value: 170 * MIB},
              ],
            ],
            [
              'mem.swap',
              [
                {ts: ts1, value: 20 * MIB},
                {ts: ts2, value: 25 * MIB},
              ],
            ],
            [
              'oom_score_adj',
              [
                {ts: ts1, value: 0},
                {ts: ts2, value: 0},
              ],
            ],
          ]),
        ],
      ]),
      processInfo: new Map([
        [
          1,
          {
            upid: 1,
            pid: 1234,
            processName: 'com.android.systemui',
            startTs: 0,
            debuggable: true,
          },
        ],
      ]),
      lmkEvents: [],
      isUserDebug: true,
      ts0: ts1,
      xMin: 0,
      xMax: 1,
    };

    const fakeSession = {isPaused: true} as unknown as LiveSession;
    const fakeApp = {} as unknown as App;
    const origGetContext = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = (() =>
      null) as unknown as typeof HTMLCanvasElement.prototype.getContext;

    try {
      m.mount(root, {
        view: () =>
          m(ProcessesTab, {
            app: fakeApp,
            data,
            session: fakeSession,
          }),
      });

      // Live mode must render LiveStripChart canvas, NOT LineChartSvg.
      expect(root.querySelector('.pf-live-strip canvas')).not.toBeNull();
      const legendItem = root.querySelector(
        '.pf-live-strip__legend-item--clickable',
      ) as HTMLElement | null;
      expect(legendItem).not.toBeNull();

      // Clicking a category legend item drills down into that category's processes.
      legendItem!.click();
      m.redraw.sync();

      expect(root.textContent).toContain('Process Memory:');
      expect(root.textContent).toContain('All categories');
      expect(root.querySelector('.pf-live-strip canvas')).not.toBeNull();
    } finally {
      HTMLCanvasElement.prototype.getContext = origGetContext;
    }
  });
});
