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

import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {ScreenshotDetailsPanel} from './screenshot_panel';

export class ScreenshotsTrack extends DatasetSliceTrack {
  static readonly kind = 'dev.perfetto.ScreenshotsTrack';

  constructor(trace: Trace, uri: string) {
    super({
      trace,
      uri,
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
        src: 'android_screenshots',
      }),
    });
  }

  override detailsPanel(_sel: TrackEventSelection) {
    return new ScreenshotDetailsPanel(this.trace.engine);
  }
}
