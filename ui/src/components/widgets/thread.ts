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
import {addEphemeralTab} from '../details/add_ephemeral_tab';
import {getThreadInfo, getThreadName, ThreadInfo} from '../sql_utils/thread';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {ThreadDetailsTab} from '../details/thread_details_tab';
import {
  createSqlIdRefRenderer,
  sqlIdRegistry,
} from './sql/details/sql_ref_renderer_registry';
import {asUtid} from '../sql_utils/core_types';
import {Utid} from '../sql_utils/core_types';
import {Trace} from '../../public/trace';

export function showThreadDetailsMenuItem(
  trace: Trace,
  utid: Utid,
  tid?: bigint,
): m.Children {
  return m(MenuItem, {
    icon: Icons.ExternalLink,
    label: 'Show thread details',
    onclick: () => {
      if (trace === undefined) return;
      addEphemeralTab(
        trace,
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

export function threadRefMenuItems(
  trace: Trace,
  info: {
    utid: Utid;
    name?: string;
    tid?: bigint;
  },
): m.Children {
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
    showThreadDetailsMenuItem(trace, info.utid, info.tid),
  ];
}

export function renderThreadRef(
  trace: Trace,
  info: {
    utid: Utid;
    name?: string;
    tid?: bigint;
  },
): m.Children {
  return m(
    PopupMenu,
    {
      trigger: m(Anchor, getThreadName(info)),
    },
    threadRefMenuItems(trace, info),
  );
}

sqlIdRegistry['thread'] = createSqlIdRefRenderer<ThreadInfo>(
  async (engine, id) => await getThreadInfo(engine, asUtid(Number(id))),
  (trace: Trace, data: ThreadInfo) => ({
    value: renderThreadRef(trace, data),
  }),
);
