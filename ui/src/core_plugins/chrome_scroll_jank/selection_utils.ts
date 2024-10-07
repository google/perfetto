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
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Trace} from '../../public/trace';

export const SCROLLS_TRACK_URI = 'perfetto.ChromeScrollJank#toplevelScrolls';
export const EVENT_LATENCY_TRACK_URI = 'perfetto.ChromeScrollJank#eventLatency';
export const JANKS_TRACK_URI = 'perfetto.ChromeScrollJank#scrollJankV3';

export function renderSliceRef(args: {
  trace: Trace;
  id: number;
  trackUri: string;
  title: m.Children;
}) {
  return m(
    Anchor,
    {
      icon: Icons.UpdateSelection,
      onclick: () => {
        args.trace.selection.selectTrackEvent(args.trackUri, args.id, {
          scrollToSelection: true,
        });
      },
    },
    args.title,
  );
}
