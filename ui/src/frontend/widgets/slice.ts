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

import {Time, duration, time} from '../../base/time';
import {SliceSqlId} from '../../trace_processor/sql_utils/core_types';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {globals} from '../globals';
import {focusHorizontalRange, verticalScrollToTrack} from '../scroll_helper';
import {BigintMath} from '../../base/bigint_math';
import {SliceDetails} from '../../trace_processor/sql_utils/slice';

interface SliceRefAttrs {
  readonly id: SliceSqlId;
  readonly name: string;
  readonly ts: time;
  readonly dur: duration;
  readonly sqlTrackId: number;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export class SliceRef implements m.ClassComponent<SliceRefAttrs> {
  view(vnode: m.Vnode<SliceRefAttrs>) {
    const switchTab = vnode.attrs.switchToCurrentSelectionTab ?? true;
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          const trackKeyByTrackId = globals.trackManager.trackKeyByTrackId;
          const trackKey = trackKeyByTrackId.get(vnode.attrs.sqlTrackId);
          if (trackKey === undefined) return;
          verticalScrollToTrack(trackKey, true);
          // Clamp duration to 1 - i.e. for instant events
          const dur = BigintMath.max(1n, vnode.attrs.dur);
          focusHorizontalRange(
            vnode.attrs.ts,
            Time.fromRaw(vnode.attrs.ts + dur),
          );

          globals.setLegacySelection(
            {
              kind: 'SLICE',
              id: vnode.attrs.id,
              trackKey,
              table: 'slice',
            },
            {
              clearSearch: true,
              pendingScrollId: undefined,
              switchToCurrentSelectionTab: switchTab,
            },
          );
        },
      },
      vnode.attrs.name,
    );
  }
}

export function sliceRef(slice: SliceDetails, name?: string): m.Child {
  return m(SliceRef, {
    id: slice.id,
    name: name ?? slice.name,
    ts: slice.ts,
    dur: slice.dur,
    sqlTrackId: slice.trackId,
  });
}
