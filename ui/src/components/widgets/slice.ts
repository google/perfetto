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
import {asSliceSqlId, SliceSqlId} from '../sql_utils/core_types';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Trace} from '../../public/trace';
import {getSlice, SliceDetails} from '../sql_utils/slice';
import {
  createSqlIdRefRenderer,
  sqlIdRegistry,
} from './sql/details/sql_ref_renderer_registry';

interface SliceRefAttrs {
  readonly trace: Trace;
  readonly id: SliceSqlId;
  readonly name: string;

  // Whether clicking on the reference should change the current tab
  // to "current selection" tab in addition to updating the selection
  // and changing the viewport. True by default.
  readonly switchToCurrentSelectionTab?: boolean;
}

export class SliceRef implements m.ClassComponent<SliceRefAttrs> {
  view(vnode: m.Vnode<SliceRefAttrs>) {
    return m(
      Anchor,
      {
        icon: Icons.UpdateSelection,
        onclick: () => {
          vnode.attrs.trace.selection.selectSqlEvent('slice', vnode.attrs.id, {
            switchToCurrentSelectionTab:
              vnode.attrs.switchToCurrentSelectionTab,
            scrollToSelection: true,
          });
        },
      },
      vnode.attrs.name,
    );
  }
}

export function sliceRef(
  trace: Trace,
  slice: SliceDetails,
  name?: string,
): m.Child {
  return m(SliceRef, {
    trace,
    id: slice.id,
    name: name ?? slice.name ?? '[null]',
  });
}

sqlIdRegistry['slice'] = createSqlIdRefRenderer<{
  slice: SliceDetails | undefined;
  id: bigint;
}>(
  async (engine, id) => {
    return {
      id,
      slice: await getSlice(engine, asSliceSqlId(Number(id))),
    };
  },
  (trace, {id, slice}) => ({
    value: slice !== undefined ? sliceRef(trace, slice) : `Unknown slice ${id}`,
  }),
);
