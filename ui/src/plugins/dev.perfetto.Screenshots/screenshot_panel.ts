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
import {exists} from '../../base/utils';
import {Engine} from '../../trace_processor/engine';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {STR} from '../../trace_processor/query_result';

export class ScreenshotDetailsPanel implements TrackEventDetailsPanel {
  private imageData?: string;

  constructor(private readonly engine: Engine) {}

  async load(selection: TrackEventSelection) {
    this.imageData = (
      await this.engine.query(`
      select extract_arg(arg_set_id, 'screenshot.jpg_image') as image_data
      from slice
      where id = ${selection.eventId}
    `)
    ).firstRow({
      image_data: STR,
    }).image_data;
  }

  render() {
    if (!exists(this.imageData)) {
      return m('h2', 'Loading Screenshot');
    }
    return m(
      '.pf-screenshot-panel',
      m('img', {
        src: 'data:image/png;base64, ' + this.imageData,
      }),
    );
  }
}
