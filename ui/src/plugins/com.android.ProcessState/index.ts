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

import './styles.scss';
import m from 'mithril';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM} from '../../trace_processor/query_result';
import {ProcessStatePage} from './page';
import {createProcessStateTrack} from './process_state_track';

const PLUGIN_ID = 'com.android.ProcessState';

// UI for the android.process_state data source: an explorer for AMS
// ProcessStateController snapshots. Every process with its oom-adj state
// (adj_type/source) and its service & content-provider bindings — answering
// "why is this process alive / at this adj?".
export default class implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(
      'SELECT count(*) AS cnt FROM android_process_state_snapshot',
    );
    if (res.iter({cnt: NUM}).cnt === 0) return;

    ctx.pages.registerPage({
      route: '/process_state',
      render: (subpage) => m(ProcessStatePage, {trace: ctx, subpage}),
    });

    // A timeline track so the snapshots are visible in context; selecting one
    // opens the compact graph in the details panel with a jump to the full page.
    const uri = '/process_state_snapshots';
    ctx.tracks.registerTrack({
      uri,
      renderer: createProcessStateTrack(ctx, uri),
    });
    ctx.defaultWorkspace.addChildInOrder(
      new TrackNode({uri, name: 'Process state', sortOrder: -50}),
    );

    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 31,
      text: 'Process state explorer',
      href: '#!/process_state',
      icon: 'account_tree',
    });
  }
}
