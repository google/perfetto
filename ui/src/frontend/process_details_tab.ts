// Copyright (C) 2024 The Android Open Source Project
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
import {Tab} from '../public/tab';
import {Upid} from '../trace_processor/sql_utils/core_types';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {Details, DetailsSchema} from './widgets/sql/details/details';
import d = DetailsSchema;
import {Trace} from '../public/trace';

export class ProcessDetailsTab implements Tab {
  private data: Details;

  // TODO(altimin): Ideally, we would not require the pid to be passed in, but
  // fetch it from the underlying data instead.
  //
  // However, the only place which creates `ProcessDetailsTab` currently is `renderProcessRef`,
  // which already has `pid` available (note that Details is already fetching the data, including
  // the `pid` from the trace processor, but it doesn't expose it for now).
  constructor(private args: {trace: Trace; upid: Upid; pid?: number}) {
    this.data = new Details(args.trace, 'process', args.upid, {
      'pid': d.Value('pid'),
      'Name': d.Value('name'),
      'Start time': d.Timestamp('start_ts', {skipIfNull: true}),
      'End time': d.Timestamp('end_ts', {skipIfNull: true}),
      'Parent process': d.SqlIdRef('process', 'parent_upid', {
        skipIfNull: true,
      }),
      'User ID': d.Value('uid', {skipIfNull: true}),
      'Android app ID': d.Value('android_appid', {skipIfNull: true}),
      'Command line': d.Value('cmdline', {skipIfNull: true}),
      'Machine id': d.Value('machine_id', {skipIfNull: true}),
      'Args': d.ArgSetId('arg_set_id'),
    });
  }

  render() {
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
      },
      m(
        GridLayout,
        m(GridLayoutColumn, m(Section, {title: 'Details'}, this.data.render())),
      ),
    );
  }

  getTitle(): string {
    if (this.args.pid !== undefined) {
      return `Process ${this.args.pid}`;
    }
    return `Process upid:${this.args.upid}`;
  }
}
