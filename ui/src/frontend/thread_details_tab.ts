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
import {Utid} from '../trace_processor/sql_utils/core_types';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {Details, DetailsSchema} from './widgets/sql/details/details';
import d = DetailsSchema;
import {Trace} from '../public/trace';

export class ThreadDetailsTab implements Tab {
  private data: Details;

  // TODO(altimin): Ideally, we would not require the tid to be passed in, but
  // fetch it from the underlying data instead. See comment in ProcessDetailsTab
  // for more details.
  constructor(private args: {trace: Trace; utid: Utid; tid?: number}) {
    this.data = new Details(args.trace, 'thread', args.utid, {
      'tid': d.Value('tid'),
      'Name': d.Value('name'),
      'Process': d.SqlIdRef('process', 'upid'),
      'Is main thread': d.Boolean('is_main_thread'),
      'Start time': d.Timestamp('start_ts', {skipIfNull: true}),
      'End time': d.Timestamp('end_ts', {skipIfNull: true}),
      'Machine id': d.Value('machine_id', {skipIfNull: true}),
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
    if (this.args.tid !== undefined) {
      return `Thread ${this.args.tid}`;
    }
    return `Thread utid:${this.args.utid}`;
  }
}
