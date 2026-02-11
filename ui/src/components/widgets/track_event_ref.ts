// Copyright (C) 2026 The Android Open Source Project
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

// This widget provides common styling for a reference which selects a track
// event (e.g. a slice) identified by a table and an ID when clicked.
export interface TrackEventRefAttrs {
  readonly trace: Trace;
  readonly table: string;
  readonly id: number;
  readonly name: string;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export class TrackEventRef implements m.ClassComponent<TrackEventRefAttrs> {
  view(vnode: m.Vnode<TrackEventRefAttrs>) {
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          vnode.attrs.trace.selection.selectSqlEvent(
            vnode.attrs.table,
            vnode.attrs.id,
            {
              switchToCurrentSelectionTab:
                vnode.attrs.switchToCurrentSelectionTab,
              scrollToSelection: true,
            },
          );
        },
      },
      vnode.attrs.name,
    );
  }
}
