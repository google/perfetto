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
import {SchedSqlId} from '../../trace_processor/sql_utils/core_types';
import {duration, time} from '../../base/time';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {globals} from '../globals';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {scrollTo} from '../../public/scroll_helper';

interface SchedRefAttrs {
  id: SchedSqlId;
  ts: time;
  dur: duration;
  cpu: number;
  // If not present, a placeholder name will be used.
  name?: string;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export function findSchedTrack(cpu: number): string | undefined {
  return globals.trackManager.findTrack((t) => {
    return t.tags?.kind === CPU_SLICE_TRACK_KIND && t.tags.cpu === cpu;
  })?.uri;
}

export function goToSchedSlice(cpu: number, id: SchedSqlId, ts: time) {
  const trackUri = findSchedTrack(cpu);
  if (trackUri === undefined) {
    return;
  }
  globals.selectionManager.selectSqlEvent('sched_slice', id);
  scrollTo({
    track: {uri: trackUri, expandGroup: true},
    time: {start: ts},
  });
}

export class SchedRef implements m.ClassComponent<SchedRefAttrs> {
  view(vnode: m.Vnode<SchedRefAttrs>) {
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          const trackUri = findSchedTrack(vnode.attrs.cpu);
          if (trackUri === undefined) return;

          globals.selectionManager.selectSqlEvent(
            'sched_slice',
            vnode.attrs.id,
            {
              switchToCurrentSelectionTab:
                vnode.attrs.switchToCurrentSelectionTab ?? true,
            },
          );
          scrollTo({
            track: {uri: trackUri, expandGroup: true},
            time: {start: vnode.attrs.ts},
          });
        },
      },
      vnode.attrs.name ?? `Sched ${vnode.attrs.id}`,
    );
  }
}
