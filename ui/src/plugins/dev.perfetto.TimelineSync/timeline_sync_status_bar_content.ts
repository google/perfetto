// Copyright (C) 2025 The Android Open Source Project
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

export type SyncStatus = 'active' | 'paused';

export interface SyncStatusbarContentAttrs {
  status: SyncStatus;
  // Callback for Resume button (when status is 'paused')
  onResume?: () => void;
  // Callback for Pause button (when status is 'active') - Button hidden if undefined
  onPause?: () => void;
  onStop?: () => void;
}

export class SyncStatusbarContent implements m.ClassComponent<SyncStatusbarContentAttrs> {
  view(vnode: m.Vnode<SyncStatusbarContentAttrs>): m.Children[] {
    const {status, onResume, onPause, onStop} = vnode.attrs;

    const isActive = status === 'active';

    const btnBaseClass = '.status-bar-button';
    const iconClass = '.material-icons';


    const pauseResumeButton = () => {
      // Only show Pause button if active AND onPause callback is provided
      if (isActive && onPause) {
        return m(`button${btnBaseClass}`, {
            onclick: onPause,
            title: 'Pause Sync',
          },
          m(`i${iconClass}`, 'pause'),
          m('span', 'Pause'),
        );
      } else if (!isActive) {
        // Show Resume button when paused
        return m(`button${btnBaseClass}`, {
            onclick: onResume,
            disabled: !onResume,
            title: 'Resume Sync',
          },
          m(`i${iconClass}`, 'play_arrow'),
          m('span', 'Resume'),
        );
      } else {
        // Don't show Pause/Resume button when inactive or if onPause is missing when active
        return null;
      }
    };

    return [
      // Left group: Description and Status
      m('div', [
        m('span.status-bar-item-container', [
          m(`i${iconClass}`, 'sync'),
          m('span', 'Timeline Sync'),
        ]),
        m('span.status-bar-separator'),
        m('span', `Status: ${status.charAt(0).toUpperCase() + status.slice(1)}`),
      ]),

      // Button group div
      m('div', [
        // Render Pause or Resume button based on state and presence of onPause
        pauseResumeButton(),

        // Stop Button
        m(`button${btnBaseClass}.text-red-600`, {
            onclick: onStop,
            disabled: !isActive, // Can stop if active or paused
            title: 'Stop Sync',
          },
          m(`i${iconClass}`, 'stop_circle'),
          m('span', 'Stop'),
        ),
      ]),
    ];
  }
}

export function createSyncStatusbarVnode(attrs: SyncStatusbarContentAttrs): m.Vnode<SyncStatusbarContentAttrs> {
  return m(SyncStatusbarContent, attrs);
}
