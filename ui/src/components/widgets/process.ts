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
import {Upid} from '../sql_utils/core_types';
import {
  getProcessInfo,
  getProcessName,
  ProcessInfo,
} from '../sql_utils/process';
import {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {ProcessDetailsTab} from '../details/process_details_tab';
import {
  createSqlIdRefRenderer,
  sqlIdRegistry,
} from '../widgets/sql/details/sql_ref_renderer_registry';
import {asUpid} from '../sql_utils/core_types';

export function showProcessDetailsMenuItem(
  trace: Trace,
  upid: Upid,
  pid?: bigint,
): m.Children {
  return m(MenuItem, {
    icon: Icons.ExternalLink,
    label: 'Show process details',
    onclick: () => {
      addEphemeralTab(
        trace,
        'processDetails',
        new ProcessDetailsTab({
          trace,
          upid,
          pid,
        }),
      );
    },
  });
}

export function processRefMenuItems(
  trace: Trace,
  info: {
    upid: Upid;
    name?: string;
    pid?: bigint;
  },
): m.Children {
  // We capture a copy to be able to pass it across async boundary to `onclick`.
  const name = info.name;
  return [
    exists(name) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy process name',
        onclick: () => copyToClipboard(name),
      }),
    exists(info.pid) &&
      m(MenuItem, {
        icon: Icons.Copy,
        label: 'Copy pid',
        onclick: () => copyToClipboard(`${info.pid}`),
      }),
    m(MenuItem, {
      icon: Icons.Copy,
      label: 'Copy upid',
      onclick: () => copyToClipboard(`${info.upid}`),
    }),
    showProcessDetailsMenuItem(trace, info.upid, info.pid),
  ];
}

export function renderProcessRef(trace: Trace, info: ProcessInfo): m.Children {
  return m(
    PopupMenu,
    {
      trigger: m(Anchor, getProcessName(info)),
    },
    processRefMenuItems(trace, info),
  );
}

sqlIdRegistry['process'] = createSqlIdRefRenderer<ProcessInfo>(
  async (engine, id) => await getProcessInfo(engine, asUpid(Number(id))),
  (trace: Trace, data: ProcessInfo) => ({
    value: renderProcessRef(trace, data),
  }),
);
