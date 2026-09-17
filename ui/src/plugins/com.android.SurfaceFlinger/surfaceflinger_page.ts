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

// The full-screen SurfaceFlinger page: a toolbar (display selector,
// snapshot time-slider, prev/next, timestamp) over the 3-pane viewer.

import './surfaceflinger.scss';
import m from 'mithril';
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import {Button} from '../../widgets/button';
import {Select} from '../../widgets/select';
import {SfViewer} from './surfaceflinger_viewer';
import type {SurfaceFlingerSession} from './surfaceflinger_session';

export interface SurfaceFlingerPageAttrs {
  readonly session: SurfaceFlingerSession;
  readonly subpage: string | undefined;
}

export class SurfaceFlingerPage
  implements m.ClassComponent<SurfaceFlingerPageAttrs>
{
  view(vnode: m.Vnode<SurfaceFlingerPageAttrs>): m.Children {
    const s = vnode.attrs.session;
    if (s.displays.length === 0) {
      return m(
        '.pf-sf-page',
        m(
          '.pf-sf-empty',
          'No SurfaceFlinger layers trace found. Record with the ' +
            'android.surfaceflinger.layers data source.',
        ),
      );
    }
    const snap = s.currentSnapshot;
    return m('.pf-sf-page', [
      m('.pf-sf-bar', [
        m(
          Select,
          {
            onchange: (e: Event) =>
              void s.setDisplay((e.target as HTMLSelectElement).value),
          },
          s.displays.map((d) =>
            m(
              'option',
              {value: d.displayId, selected: d.displayId === s.displayId},
              d.isVirtual ? `${d.displayName} (virtual)` : d.displayName,
            ),
          ),
        ),
        m(Button, {
          icon: 'timeline',
          label: 'Timeline',
          title: 'Jump to this snapshot in the timeline',
          onclick: () => {
            const cur = s.currentSnapshot;
            if (cur === undefined) return;
            s.trace.navigate('#!/viewer');
            // Select the snapshot slice on the current display's track (opens its
            // details panel) and scroll the timeline to it.
            s.trace.selection.selectTrackEvent(
              `/surfaceflinger_track/${s.displayId}`,
              cur.snapshotId,
              {scrollToSelection: true},
            );
          },
        }),
        m(Button, {
          icon: 'first_page',
          title: 'First snapshot',
          onclick: () => void s.setIndex(0),
        }),
        m(Button, {
          icon: 'chevron_left',
          title: 'Previous snapshot',
          onclick: () => void s.setIndex(s.index - 1),
        }),
        m('input.pf-sf-scrub[type=range]', {
          min: 0,
          max: Math.max(0, s.snapshots.length - 1),
          value: s.index,
          oninput: (e: Event) =>
            void s.setIndex(Number((e.target as HTMLInputElement).value)),
        }),
        m(Button, {
          icon: 'chevron_right',
          title: 'Next snapshot',
          onclick: () => void s.setIndex(s.index + 1),
        }),
        m(Button, {
          icon: 'last_page',
          title: 'Last snapshot',
          onclick: () => void s.setIndex(s.snapshots.length - 1),
        }),
        m(
          'span.pf-sf-bar__pos',
          `${s.snapshots.length === 0 ? 0 : s.index + 1} / ${s.snapshots.length}`,
        ),
        snap !== undefined
          ? m('span.pf-sf-bar__ts', [
              m(Timestamp, {trace: s.trace, ts: Time.fromRaw(snap.ts)}),
            ])
          : null,
      ]),
      m(
        '.pf-sf-main',
        s.snapshots.length === 0
          ? m('.pf-sf-empty', 'No snapshots for this display.')
          : m(SfViewer, {session: s}),
      ),
    ]);
  }
}
