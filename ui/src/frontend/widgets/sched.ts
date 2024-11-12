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
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {AppImpl} from '../../core/app_impl';

interface SchedRefAttrs {
  // The id of the referenced sched slice in the sched_slice table.
  readonly id: SchedSqlId;

  // If not present, a placeholder name will be used.
  readonly name?: string;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export function goToSchedSlice(id: SchedSqlId) {
  // TODO(primiano): the Trace object should be properly injected here.
  AppImpl.instance.trace?.selection.selectSqlEvent('sched_slice', id, {
    scrollToSelection: true,
  });
}

export class SchedRef implements m.ClassComponent<SchedRefAttrs> {
  view(vnode: m.Vnode<SchedRefAttrs>) {
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          // TODO(primiano): the Trace object should be properly injected here.
          AppImpl.instance.trace?.selection.selectSqlEvent(
            'sched_slice',
            vnode.attrs.id,
            {
              switchToCurrentSelectionTab:
                vnode.attrs.switchToCurrentSelectionTab ?? true,
              scrollToSelection: true,
            },
          );
        },
      },
      vnode.attrs.name ?? `Sched ${vnode.attrs.id}`,
    );
  }
}
