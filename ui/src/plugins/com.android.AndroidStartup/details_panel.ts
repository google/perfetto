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
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import {exists} from '../../base/utils';
import {NUM_NULL} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {findMainThreadTrackUri, scrollToTrackAndSelect} from './navigate';

interface StartupInfo {
  package: string;
  startupType: string | null;
  upid: number | null;
  mainThreadTrackId: number | null;
}

export class StartupDetailsPanel implements TrackEventDetailsPanel {
  private startup?: StartupInfo;
  private selection?: TrackEventSelection;
  private isLoading = true;

  constructor(private readonly trace: Trace) {}

  async load(sel: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    this.selection = sel;

    // The selection object is enriched with dataset columns by
    // getSelectionDetails(). Extract the startup-specific fields.
    const extra = sel as unknown as {
      name: string;
      startup_type: string | null;
      upid: number | null;
    };

    // Query for the main thread track ID so we can navigate like the deeplink.
    let mainThreadTrackId: number | null = null;
    if (extra.upid != null) {
      const result = await this.trace.engine.query(`
        SELECT tt.id AS main_thread_track_id
        FROM thread t
        JOIN thread_track tt ON t.utid = tt.utid
        WHERE t.upid = ${extra.upid} AND t.is_main_thread = 1
        LIMIT 1
      `);
      const it = result.iter({main_thread_track_id: NUM_NULL});
      if (it.valid()) {
        mainThreadTrackId = it.main_thread_track_id;
      }
    }

    this.startup = {
      package: extra.name,
      startupType: extra.startup_type,
      upid: extra.upid,
      mainThreadTrackId,
    };

    this.isLoading = false;
  }

  render(): m.Children {
    if (this.isLoading || !this.startup || !this.selection) {
      return m(DetailsShell, {
        title: 'Android App Startup',
        description: 'Loading...',
      });
    }

    const {package: pkg, startupType, upid} = this.startup;
    const sel = this.selection;

    return m(
      DetailsShell,
      {title: 'Android App Startup'},
      m(
        Section,
        {title: 'Details'},
        m(
          Tree,
          m(TreeNode, {left: 'Package', right: pkg}),
          exists(startupType) &&
            m(TreeNode, {left: 'Startup Type', right: startupType}),
          m(TreeNode, {
            left: 'Start time',
            right: m(Timestamp, {trace: this.trace, ts: sel.ts}),
          }),
          exists(sel.dur) &&
            sel.dur > 0n &&
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {trace: this.trace, dur: sel.dur}),
            }),
        ),
      ),
      upid != null &&
        m(
          Section,
          {title: 'Actions'},
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () => this.goToProcess(),
              title: 'Go to process',
            },
            'Go to process',
          ),
        ),
    );
  }

  private goToProcess() {
    if (this.startup?.upid == null || !this.selection) return;

    const processGroups = this.trace.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    ) as ProcessThreadGroupsPlugin;

    const group = processGroups.getGroupForProcess(this.startup.upid);
    if (!group?.uri) return;

    group.expand();

    const tracksToSelect: string[] = [];
    if (this.startup.mainThreadTrackId != null) {
      const mainThreadUri = findMainThreadTrackUri(
        this.trace,
        this.startup.mainThreadTrackId,
      );
      if (mainThreadUri) {
        tracksToSelect.push(mainThreadUri);
      }
    }

    scrollToTrackAndSelect(
      this.trace,
      group.uri,
      tracksToSelect,
      this.selection.ts,
      this.selection.dur ?? 0n,
    );
  }
}
