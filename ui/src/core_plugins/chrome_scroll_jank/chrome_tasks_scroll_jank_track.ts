// Copyright (C) 2022 The Android Open Source Project
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

import {
  NAMED_ROW,
  NamedRow,
  NamedSliceTrack,
} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {Slice} from '../../public/track';

export class ChromeTasksScrollJankTrack extends NamedSliceTrack {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  rowToSlice(row: NamedRow): Slice {
    return this.rowToSliceBase(row);
  }

  getSqlSource(): string {
    return `
      select
        s2.ts as ts,
        s2.dur as dur,
        s2.id as id,
        0 as depth,
        s1.full_name as name
      from chrome_tasks_delaying_input_processing s1
      join slice s2 on s2.id=s1.slice_id
    `;
  }
}
