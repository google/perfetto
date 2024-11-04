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
import {ThreadStateSqlId} from '../../trace_processor/sql_utils/core_types';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {ThreadState} from '../../trace_processor/sql_utils/thread_state';
import {AppImpl} from '../../core/app_impl';

interface ThreadStateRefAttrs {
  id: ThreadStateSqlId;
  // If not present, a placeholder name will be used.
  name?: string;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export class ThreadStateRef implements m.ClassComponent<ThreadStateRefAttrs> {
  view(vnode: m.Vnode<ThreadStateRefAttrs>) {
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          // TODO(primiano): the Trace object should be properly injected here.
          AppImpl.instance.trace?.selection.selectSqlEvent(
            'thread_state',
            vnode.attrs.id,
            {
              switchToCurrentSelectionTab:
                vnode.attrs.switchToCurrentSelectionTab,
              scrollToSelection: true,
            },
          );
        },
      },
      vnode.attrs.name ?? `Thread State ${vnode.attrs.id}`,
    );
  }
}

export function threadStateRef(state: ThreadState): m.Child {
  if (state.thread === undefined) return null;

  return m(ThreadStateRef, {
    id: state.threadStateSqlId,
  });
}
