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
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import {addEphemeralTab} from '../../common/add_ephemeral_tab';
import {
  getThreadInfo,
  getThreadName,
  ThreadInfo,
} from '../../trace_processor/sql_utils/thread';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu2} from '../../widgets/menu';
import {ThreadDetailsTab} from '../thread_details_tab';
import {
  createSqlIdRefRenderer,
  sqlIdRegistry,
} from './sql/details/sql_ref_renderer_registry';
import {asUtid} from '../../trace_processor/sql_utils/core_types';
import {Utid} from '../../trace_processor/sql_utils/core_types';
import {AppImpl} from '../../core/app_impl';

export function showThreadDetailsMenuItem(
  utid: Utid,
  tid?: number,
): m.Children {
  return m(MenuItem, {
    icon: Icons.ExternalLink,
    label: 'Show thread details',
    onclick: () => {
      // TODO(primiano): `trace` should be injected, but doing so would require
      // an invasive refactoring of most classes in frontend/widgets/sql/*.
      const trace = AppImpl.instance.trace;
      if (trace === undefined) return;
      addEphemeralTab(
        'threadDetails',
        new ThreadDetailsTab({
          trace,
          utid,
          tid,
        }),
      );
    },
  });
}

export function threadRefMenuItems(info: {
  utid: Utid;
  name?: string;
  tid?: number;
}): m.Children {
  // We capture a copy to be able to pass it across async boundary to `onclick`.
  const name = info.name;
  return [
    exists(name) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy thread name',
        onclick: () => copyToClipboard(name),
      }),
    exists(info.tid) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy tid',
        onclick: () => copyToClipboard(`${info.tid}`),
      }),
    m(MenuItem, {
      icon: Icons.Copy,
      label: 'Copy utid',
      onclick: () => copyToClipboard(`${info.utid}`),
    }),
    showThreadDetailsMenuItem(info.utid, info.tid),
  ];
}

export function renderThreadRef(info: {
  utid: Utid;
  name?: string;
  tid?: number;
}): m.Children {
  return m(
    PopupMenu2,
    {
      trigger: m(Anchor, getThreadName(info)),
    },
    threadRefMenuItems(info),
  );
}

sqlIdRegistry['thread'] = createSqlIdRefRenderer<ThreadInfo>(
  async (engine, id) => await getThreadInfo(engine, asUtid(Number(id))),
  (data: ThreadInfo) => ({
    value: renderThreadRef(data),
  }),
);
