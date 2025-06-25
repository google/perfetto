// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Duration, Time} from '../../base/time';
import {TrackRenderContext, TrackRenderer} from '../../public/track';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {ElementDetailsPanel} from './details';

export class LynxElementTrack implements TrackRenderer {
  getHeight(): number {
    return 0;
  }

  render(_ctx: TrackRenderContext): void {}

  async getSelectionDetails(
    _id: number,
  ): Promise<TrackEventDetails | undefined> {
    return {
      ts: Time.fromRaw(BigInt(0)),
      dur: Duration.fromRaw(BigInt(0)),
    };
  }

  detailsPanel(_: TrackEventSelection): TrackEventDetailsPanel {
    return new ElementDetailsPanel();
  }
}
