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
import {renderTrackSettingMenu} from '../../../components/track_settings_renderer';
import {Trace} from '../../../public/trace';
import {Track} from '../../../public/track';
import {TrackNode, Workspace} from '../../../public/workspace';
import {MenuDivider, MenuItem, MenuTitle} from '../../../widgets/menu';
import {TrackDetailsMenu} from './track_details_menu';

interface TrackPopupMenuAttrs {
  readonly trace: Trace;
  readonly node: TrackNode;
  readonly descriptor?: Track;
}

// This component contains the track menu items which are displayed inside a
// popup menu on each track. They're in a component to avoid having to render
// them every single mithril cycle.
export const TrackMenu = {
  view({attrs}: m.Vnode<TrackPopupMenuAttrs>) {
    const {trace, node, descriptor} = attrs;
    return [
      m(MenuItem, {
        label: 'Select track',
        icon: 'select',
        disabled: !node.uri,
        onclick: () => {
          trace.selection.selectTrack(node.uri!);
        },
        title: node.uri
          ? 'Select track'
          : 'Track has no URI and cannot be selected',
      }),
      m(
        MenuItem,
        {label: 'Track details', icon: 'info'},
        m(TrackDetailsMenu, {node: node, descriptor: descriptor}),
      ),
      m(MenuDivider),
      m(
        MenuItem,
        {label: 'Copy to workspace', icon: 'content_copy'},
        trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: () => copyToWorkspace(trace, node, ws),
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          icon: 'add',
          onclick: () => copyToWorkspace(trace, node),
        }),
      ),
      m(
        MenuItem,
        {label: 'Copy & switch to workspace', icon: 'content_copy'},
        trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: async () => {
              copyToWorkspace(trace, node, ws);
              trace.workspaces.switchWorkspace(ws);
            },
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          icon: 'add',
          onclick: async () => {
            const ws = copyToWorkspace(trace, node);
            trace.workspaces.switchWorkspace(ws);
          },
        }),
      ),
      m(MenuDivider),
      m(MenuItem, {
        label: 'Rename',
        icon: 'edit',
        disabled: !node.workspace?.userEditable,
        onclick: async () => {
          const newName = await trace.omnibox.prompt('New name');
          if (newName) {
            node.name = newName;
          }
        },
      }),
      m(MenuItem, {
        label: 'Remove',
        icon: 'delete',
        disabled: !node.workspace?.userEditable,
        onclick: () => {
          node.remove();
        },
      }),
      ...renderTrackSettings(descriptor),
    ];
  },
};

function renderTrackSettings(descriptor?: Track): m.Children[] {
  const settings = descriptor?.renderer.settings;
  if (!settings || settings.length === 0) return [];
  return [
    m(MenuDivider),
    m(MenuTitle, {label: 'Settings'}),
    ...settings.map((setting) =>
      renderTrackSettingMenu(setting.descriptor, (v) => setting.update(v), [
        setting.value,
      ]),
    ),
  ];
}

function copyToWorkspace(trace: Trace, node: TrackNode, ws?: Workspace) {
  // If no workspace provided, create a new one.
  if (!ws) {
    ws = trace.workspaces.createEmptyWorkspace('Untitled Workspace');
  }
  // Deep clone makes sure all group's content is also copied
  const newNode = node.clone(true);
  newNode.removable = true;
  ws.addChildLast(newNode);
  return ws;
}
