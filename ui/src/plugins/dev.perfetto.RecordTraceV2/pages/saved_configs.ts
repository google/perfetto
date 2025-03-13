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
import {RecordingManager} from '../recording_manager';
import {RecordSubpage} from '../config/config_interfaces';
import {SavedSessionSchema, RecordPluginSchema} from '../serialization_schema';
import {assertExists} from '../../../base/logging';
import {shareRecordConfig} from '../config/config_sharing';

export function savedConfigsPage(recMgr: RecordingManager): RecordSubpage {
  const savedConfigs = new Array<SavedSessionSchema>();

  return {
    kind: 'GLOBAL_PAGE',
    id: 'configs',
    icon: 'save',
    title: 'Saved configs',
    subtitle: 'Save, restore and export configs',
    render() {
      return m(SavedConfigsPage, {recMgr, savedConfigs});
    },
    serialize(state: RecordPluginSchema) {
      state.savedSessions = [...savedConfigs];
    },
    deserialize(state: RecordPluginSchema) {
      savedConfigs.splice(0);
      savedConfigs.push(...state.savedSessions);
    },
  };
}

type RecMgrAttrs = {
  recMgr: RecordingManager;
  savedConfigs: Array<SavedSessionSchema>;
};

class SavedConfigsPage implements m.ClassComponent<RecMgrAttrs> {
  private newConfigName = '';
  private recMgr: RecordingManager;
  private savedConfigs: Array<SavedSessionSchema>;

  constructor({attrs}: m.CVnode<RecMgrAttrs>) {
    this.recMgr = attrs.recMgr;
    this.savedConfigs = attrs.savedConfigs;
  }

  view() {
    const canSave =
      this.newConfigName.length > 0 &&
      this.savedConfigs.every((s) => s.name !== this.newConfigName);
    return [
      m('header', 'Save and load configurations'),
      m('.input-config', [
        m('input', {
          value: this.newConfigName,
          placeholder: 'Title for config',
          oninput: (e: Event) => {
            this.newConfigName = (e.target as HTMLInputElement).value;
          },
        }),
        m(
          'button',
          {
            class: 'config-button',
            disabled: !canSave,
            title: canSave
              ? 'Save current config'
              : 'Duplicate name, saving disabled',
            onclick: () => {
              this.savedConfigs.push({
                name: this.newConfigName,
                config: this.recMgr.serializeSession(),
              });
              this.newConfigName = '';
            },
          },
          m('i.material-icons', 'save'),
        ),
      ]),
      this.savedConfigs.map((s) => this.renderSavedSessions(s)),
    ];
  }

  private renderSavedSessions(item: SavedSessionSchema) {
    const self = this;
    return m('.config', [
      m('span.title-config', item.name),
      m(
        'button',
        {
          class: 'config-button',
          title: 'Apply configuration settings',
          onclick: () => {
            this.recMgr.loadSession(item.config);
          },
        },
        m('i.material-icons', 'file_upload'),
      ),
      m(
        'button',
        {
          class: 'config-button',
          title: 'Overwrite configuration with current settings',
          onclick: () => {
            const msg = `Overwrite config "${item.name}" with current settings?`;
            if (!confirm(msg)) return;
            const savedCfg = assertExists(
              this.savedConfigs.find((s) => s.name === item.name),
            );
            savedCfg.config = this.recMgr.serializeSession();
          },
        },
        m('i.material-icons', 'save'),
      ),
      m(
        'button',
        {
          class: 'config-button',
          title: 'Generate a shareable URL for the saved config',
          onclick: () => shareRecordConfig(item.config),
        },
        m('i.material-icons', 'share'),
      ),
      m(
        'button',
        {
          class: 'config-button',
          title: 'Remove configuration',
          onclick: () => {
            const idx = this.savedConfigs.findIndex(
              (s) => s.name === item.name,
            );
            if (idx < 0) return;
            self.savedConfigs.splice(idx, 1);
          },
        },
        m('i.material-icons', 'delete'),
      ),
    ]);
  }
}
