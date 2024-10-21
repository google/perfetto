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
import {
  Details,
  DetailsSchema,
} from '../../frontend/widgets/sql/details/details';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import d = DetailsSchema;

export class ChromeTasksDetailsPanel implements TrackEventDetailsPanel {
  private readonly data: Details;

  constructor(trace: Trace, eventId: number) {
    this.data = new Details(trace, 'chrome_tasks', eventId, {
      'Task name': 'name',
      'Start time': d.Timestamp('ts'),
      'Duration': d.Interval('ts', 'dur'),
      'Process': d.SqlIdRef('process', 'upid'),
      'Thread': d.SqlIdRef('thread', 'utid'),
      'Slice': d.SqlIdRef('slice', 'id'),
    });
  }

  render() {
    return m(
      DetailsShell,
      {
        title: 'Chrome Tasks',
      },
      m(GridLayout, m(GridLayoutColumn, this.data.render())),
    );
  }
}
