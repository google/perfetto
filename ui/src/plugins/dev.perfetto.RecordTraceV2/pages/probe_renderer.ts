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
import {assetSrc} from '../../../base/assets';
import {ConfigManager} from '../config/config_manager';
import {RecordProbe} from '../config/config_interfaces';
import {exists} from '../../../base/utils';
import {DocsChip} from './widgets/docs_chip';
import {classNames} from '../../../base/classnames';

export interface ProbeAttrs {
  cfgMgr: ConfigManager;
  probe: RecordProbe;
}

export class Probe implements m.ClassComponent<ProbeAttrs> {
  view({attrs}: m.CVnode<ProbeAttrs>) {
    const onToggle = (enabled: boolean) => {
      attrs.cfgMgr.setProbeEnabled(attrs.probe.id, enabled);
    };

    const probe = attrs.probe;
    const forceEnabledDeps = attrs.cfgMgr.getProbeEnableDependants(
      attrs.probe.id,
    );
    const enabled = attrs.cfgMgr.isProbeEnabled(attrs.probe.id);
    const compact =
      !exists(probe.description) &&
      !exists(probe.image) &&
      (probe.settings ?? []).length === 0;
    return m(
      '.probe',
      {
        className: classNames(enabled && 'enabled', compact && 'compact'),
      },
      probe.image &&
        m('img', {
          src: assetSrc(`assets/${probe.image}`),
          onclick: () => onToggle(!enabled),
        }),
      m(
        'label',
        m(`input[type=checkbox]`, {
          checked: enabled,
          disabled: forceEnabledDeps.length > 0,
          title:
            forceEnabledDeps.length > 0
              ? 'Force-enabled due to ' + forceEnabledDeps.join(',')
              : '',
          oninput: (e: InputEvent) => {
            onToggle((e.target as HTMLInputElement).checked);
          },
        }),
        m('span', probe.title),
      ),
      compact
        ? ''
        : m(
            `div${probe.image ? '' : '.extended-desc'}`,
            m(
              'div',
              probe.description,
              probe.docsLink && m(DocsChip, {href: probe.docsLink}),
            ),
            m(
              '.probe-config',
              Object.values(attrs.probe.settings ?? {}).map((widget) =>
                widget.render(),
              ),
            ),
          ),
    );
  }
}
