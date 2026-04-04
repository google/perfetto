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
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {DurationWidget} from '../../components/widgets/duration';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {
  AndroidLockContentionEventSource,
  LockContentionDetails,
} from './android_lock_contention_event_source';

export class LockOwnerDetailsPanel implements TrackEventDetailsPanel {
  private richDetails?: LockContentionDetails;

  constructor(
    private readonly trace: Trace,
    private readonly eventId: number,
  ) {}

  async load() {
    const trackUri = 'com.android.AndroidLockContention#OwnerEvents';
    const source = new AndroidLockContentionEventSource(this.trace);
    this.richDetails =
      (await source.fetchDetails(this.eventId, trackUri)) ?? undefined;
  }

  render() {
    if (this.richDetails === undefined) {
      return m(DetailsShell, {title: 'Lock Owner', description: 'Loading...'});
    }

    const row = this.richDetails;
    return m(
      DetailsShell,
      {
        title: 'Lock Owner Contention',
        description: row.lockName,
      },
      row.parentId !== null &&
        m(
          Callout,
          {
            intent: Intent.Warning,
            icon: 'warning',
          },
          m('strong', 'Nested Contention Warning: '),
          'The thread holding this lock is currently blocked by another lock! ',
        ),
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Blocked Thread (Victim)'},
            m(
              Tree,
              m(TreeNode, {
                left: 'Thread',
                right: `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}]`,
              }),
              m(TreeNode, {
                left: 'Main Thread',
                right: row.isBlockedThreadMain ? 'Yes' : 'No',
              }),
              m(TreeNode, {left: 'Method', right: row.blockedMethod}),
              m(TreeNode, {left: 'Location', right: row.blockedSrc}),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Contention Details'},
            m(
              Tree,
              m(TreeNode, {left: 'Lock', right: row.lockName}),
              m(TreeNode, {
                left: 'Duration',
                right:
                  row.dur !== null
                    ? m(DurationWidget, {dur: row.dur, trace: this.trace})
                    : '-',
              }),
              m(TreeNode, {
                left: 'Monotonic Duration',
                right:
                  row.monotonicDur !== null
                    ? m(DurationWidget, {
                        dur: row.monotonicDur,
                        trace: this.trace,
                      })
                    : '-',
              }),
              m(TreeNode, {left: 'Other Waiters', right: `${row.waiterCount}`}),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Blocking Thread (Culprit)'},
            m(
              Tree,
              m(TreeNode, {
                left: 'Thread',
                right: `${row.blockingThreadName} [${row.blockingThreadTid ?? '-'}]`,
              }),
              m(TreeNode, {
                left: 'Main Thread',
                right: row.isBlockingThreadMain ? 'Yes' : 'No',
              }),
              m(TreeNode, {left: 'Method', right: row.blockingMethod}),
              m(TreeNode, {left: 'Location', right: row.blockingSrc}),
            ),
          ),
        ),
      ),
    );
  }
}
