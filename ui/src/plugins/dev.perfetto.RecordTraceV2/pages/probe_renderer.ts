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
import {classNames} from '../../../base/classnames';
import {Anchor} from '../../../widgets/anchor';
import {Icons} from '../../../base/semantic_icons';
import {Switch} from '../../../widgets/switch';

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
      '.pf-probe',
      {
        className: classNames(enabled && 'enabled', compact && 'compact'),
      },
      probe.image &&
        m('img', {
          src: assetSrc(`assets/${probe.image}`),
          onclick: () => onToggle(!enabled),
        }),
      m(Switch, {
        className: 'pf-probe__switch',
        checked: enabled,
        disabled: forceEnabledDeps.length > 0,
        title:
          forceEnabledDeps.length > 0
            ? 'Force-enabled due to ' + forceEnabledDeps.join(',')
            : '',
        oninput: (e: InputEvent) => {
          onToggle((e.target as HTMLInputElement).checked);
        },
        label: probe.title,
      }),
      compact
        ? ''
        : m(
            `div${probe.image ? '' : '.extended-desc'}`,
            probe.description &&
              m(
                '.pf-probe__descr',
                formatDescription(probe.description),
                probe.docsLink &&
                  m(
                    Anchor,
                    {icon: Icons.ExternalLink, href: probe.docsLink},
                    'Docs',
                  ),
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

/** Formats the probe.description turning ``` blocks into code snippets */
function formatDescription(input: string | undefined): m.Children {
  if (input === undefined) return [];

  const result: m.Children = [];
  const regex = /```(.*?)```/gs;
  let lastIndex = 0;

  for (const match of input.matchAll(regex)) {
    const [fullMatch, codeContent] = match;
    const matchStart = match.index ?? 0;

    // Add preceding plain text
    if (matchStart > lastIndex) {
      const text = input.slice(lastIndex, matchStart);
      result.push(m('div', text));
    }

    // Add code block
    result.push(m('code', codeContent));

    lastIndex = matchStart + fullMatch.length;
  }

  // Add remaining text after last match
  if (lastIndex < input.length) {
    result.push(m('div', input.slice(lastIndex)));
  }

  return result;
}
