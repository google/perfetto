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
import type {
  LineChartData,
  LineChartSeries,
} from '../../../components/widgets/charts/line_chart';
import type {App} from '../../../public/app';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {Intent} from '../../../widgets/common';
import {MenuDivider, MenuItem, PopupMenu} from '../../../widgets/menu';
import {PopupPosition} from '../../../widgets/popup';
import {Stack} from '../../../widgets/stack';
import type {
  LiveSession,
  ProfileView,
  SnapshotData,
} from '../sessions/live_session';
import {
  beginPendingMemoryCapture,
  failPendingMemoryCapture,
  memoryLoadingHref,
} from '../memory_explorer/loading_state';
import './live_chrome.scss';
import {ProfilePage} from './profile_page';

// Shared chrome for live pages: the session pill (device, disconnect, stats)
// and the in-progress heap profile view.

export interface LiveSessionAttrs {
  readonly app: App;
  readonly session: LiveSession;
  readonly onStopped: () => void;
}

export function buildProcessMemoryBreakdown(
  data: SnapshotData,
  pid: number,
  t0: number,
): LineChartData | undefined {
  // SnapshotData keys process counters by upid; resolve from pid.
  let upid: number | undefined;
  for (const info of data.processInfo.values()) {
    if (info.pid === pid) {
      upid = info.upid;
      break;
    }
  }
  if (upid === undefined) return undefined;
  const pidCounters = data.processCountersByUpid.get(upid);
  if (pidCounters === undefined) return undefined;
  const SERIES_NAMES = ['RSS Anon + Swap', 'File', 'DMA-BUF'] as const;
  const counterMapping: Record<string, string> = {
    'mem.rss.anon': 'RSS Anon + Swap',
    'mem.swap': 'RSS Anon + Swap',
    'mem.rss.file': 'File',
    'mem.dmabuf_rss': 'DMA-BUF',
  };
  const tsSet = new Set<number>();
  const bySeriesTs = new Map<number, Map<string, number>>();
  for (const [counterName, samples] of pidCounters) {
    const seriesName = counterMapping[counterName];
    if (seriesName === undefined) continue;
    for (const {ts, value} of samples) {
      tsSet.add(ts);
      let seriesMap = bySeriesTs.get(ts);
      if (seriesMap === undefined) {
        seriesMap = new Map();
        bySeriesTs.set(ts, seriesMap);
      }
      seriesMap.set(
        seriesName,
        (seriesMap.get(seriesName) ?? 0) + Math.round(value / 1024),
      );
    }
  }
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;
  const colors: Record<string, string> = {
    'RSS Anon + Swap': 'var(--pf-color-warning)',
    'File': 'var(--pf-color-success)',
    'DMA-BUF': 'var(--pf-color-primary)',
  };
  const series: LineChartSeries[] = [];
  for (const name of SERIES_NAMES) {
    const points = timestamps.map((ts) => ({
      x: (ts - t0) / 1e9,
      y: bySeriesTs.get(ts)?.get(name) ?? 0,
    }));
    if (points.some((p) => p.y > 0)) {
      series.push({name, points, color: colors[name]});
    }
  }
  if (series.length === 0) return undefined;
  return {series};
}

export class SessionPill implements m.ClassComponent<LiveSessionAttrs> {
  view({attrs}: m.CVnode<LiveSessionAttrs>) {
    return this.renderSessionPill(attrs);
  }

  private renderSessionPill(attrs: LiveSessionAttrs): m.Children {
    const {session} = attrs;
    return m(
      '.pf-memscope-session-pill',
      // Device identity sits to the left of the button group.
      m(
        '.pf-memscope-session-pill__device',
        m('span', session.deviceName),
        session.data?.isUserDebug &&
          m(Chip, {label: 'userdebug', intent: Intent.Warning}),
      ),
      m(
        Stack,
        {
          orientation: 'horizontal',
        },
        m(Button, {
          variant: ButtonVariant.Filled,
          label: 'Disconnect',
          icon: 'usb_off',
          intent: Intent.Danger,
          onclick: () => attrs.onStopped(),
        }),
        // Overflow menu for session actions.
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              icon: 'more_vert',
            }),
            position: PopupPosition.BottomEnd,
          },

          m(MenuItem, {
            label: 'Stop & open trace',
            icon: 'open_in_new',
            disabled: session.lastTraceBuffer === undefined,
            onclick: () => this.stopAndOpenTrace(attrs),
          }),
          m(MenuDivider),
          m(
            MenuItem,
            {
              label: 'Stats for nerds',
              icon: 'photo_camera',
            },
            this.renderSnapshotStats(session),
          ),
        ),
      ),
    );
  }

  private renderSnapshotStats(session: LiveSession): m.Children {
    if (session.lastSnapshotMs <= 0) {
      return m(
        '.pf-memscope-snapshot-info',
        m('.pf-memscope-snapshot-info__empty', 'Waiting for snapshot…'),
      );
    }
    return m(
      '.pf-memscope-snapshot-info',
      m('.pf-memscope-snapshot-info__heading', 'Snapshot'),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Count'),
        m('span', `#${session.snapshotCount}`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Size'),
        m('span', `${session.lastSnapshotSizeKb.toFixed(0)}kB`),
      ),
      session.lastBufferUsagePct !== undefined &&
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'Buffer usage'),
          m('span', `${session.lastBufferUsagePct.toFixed(1)}%`),
        ),
      session.data !== undefined && [
        m('.pf-memscope-snapshot-info__heading', 'Counter range'),
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'First sample'),
          m('span', `${session.data.xMin.toFixed(1)}s`),
        ),
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'Last sample'),
          m('span', `${session.data.xMax.toFixed(1)}s`),
        ),
      ],
      m('.pf-memscope-snapshot-info__heading', 'Timings'),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Clone'),
        m('span', `${session.lastCloneMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Parse'),
        m('span', `${session.lastParseMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Query'),
        m('span', `${session.lastQueryMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Extract'),
        m('span', `${session.lastExtractMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row.pf-memscope-snapshot-info__sum',
        m('span', 'Total'),
        m('span', `${session.lastSnapshotMs}ms`),
      ),
      session.snapshotOverrun &&
        m(
          '.pf-memscope-snapshot-info__overrun',
          m('span.material-icons', 'warning'),
          'Exceeds interval — increase snapshot rate',
        ),
    );
  }

  private stopAndOpenTrace(attrs: LiveSessionAttrs) {
    const buffer = attrs.session.lastTraceBuffer;
    if (buffer === undefined) return;
    const fileName = `live-memory-${Date.now()}.perfetto-trace`;
    beginPendingMemoryCapture({
      title: fileName,
      fromLive: true,
      deviceName: attrs.session.deviceName,
      stage: 'opening',
      capturedBytes: buffer.byteLength,
      detail: 'Starting Trace Processor…',
    });
    attrs.app.navigate(memoryLoadingHref());
    attrs.onStopped();
    void attrs.app
      .openTraceFromBuffer({buffer, title: fileName, fileName})
      .catch((e) => {
        failPendingMemoryCapture(e instanceof Error ? e.message : String(e));
        attrs.app.raf.scheduleFullRedraw();
      });
  }
}

export class LiveProfileView implements m.ClassComponent<LiveSessionAttrs> {
  private profileBaseline?: {anonSwap: number; file: number; dmabuf: number};

  view({attrs}: m.CVnode<LiveSessionAttrs>) {
    const profile = attrs.session.profile;
    if (profile === undefined) return undefined;
    return this.renderProfilePage(attrs, profile);
  }

  private renderProfilePage(
    attrs: LiveSessionAttrs,
    profile: ProfileView,
  ): m.Children {
    const {session} = attrs;
    const data = session.data;
    const t0 = data?.ts0 ?? 0;
    const chartData = data
      ? buildProcessMemoryBreakdown(data, profile.pid, t0)
      : undefined;

    // Capture baseline from first chart data.
    if (this.profileBaseline === undefined && chartData !== undefined) {
      const first = (name: string): number => {
        const s = chartData.series.find(
          (sr: LineChartSeries) => sr.name === name,
        );
        return s !== undefined && s.points.length > 0 ? s.points[0]!.y : 0;
      };
      this.profileBaseline = {
        anonSwap: first('RSS Anon + Swap'),
        file: first('File'),
        dmabuf: first('DMA-BUF'),
      };
    }

    return m(ProfilePage, {
      state: profile.state as 'recording' | 'stopping' | 'finished',
      bufferUsagePct: profile.bufferUsagePct,
      processName: profile.processName,
      pid: profile.pid,
      startMs: profile.startMs,
      chartData,
      baseSec: t0 / 1e9,
      baseline: this.profileBaseline,
      onStop: () => {
        session.stopAndOpenProfile().then(() => {
          this.profileBaseline = undefined;
          m.redraw();
        });
      },
      onStopAndDownload: () => {
        session.stopAndDownloadProfile().then(() => {
          this.profileBaseline = undefined;
          m.redraw();
        });
      },
      onCancel: () => {
        session.cancelProfile().then(() => {
          this.profileBaseline = undefined;
          m.redraw();
        });
      },
    });
  }
}
