// Copyright (C) 2023 The Android Open Source Project
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
import {assertTrue} from '../../base/logging';
import {exists} from '../../base/utils';
import {getSlice, SliceDetails} from '../../trace_processor/sql_utils/slice';
import {asSliceSqlId} from '../../trace_processor/sql_utils/core_types';
import {Engine} from '../../trace_processor/engine';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';

export class ScreenshotDetailsPanel implements TrackEventDetailsPanel {
  private sliceDetails?: SliceDetails;

  constructor(private readonly engine: Engine) {}

  async load(selection: TrackEventSelection) {
    this.sliceDetails = await getSlice(
      this.engine,
      asSliceSqlId(selection.eventId),
    );
  }

  render() {
    if (
      !exists(this.sliceDetails) ||
      !exists(this.sliceDetails.args) ||
      this.sliceDetails.args.length == 0
    ) {
      return m('h2', 'Loading Screenshot');
    }
    assertTrue(this.sliceDetails.args[0].key == 'screenshot.jpg_image');
    return m(
      '.screenshot-panel',
      m('img', {
        src: 'data:image/png;base64, ' + this.sliceDetails.args[0].displayValue,
      }),
    );
  }
}
