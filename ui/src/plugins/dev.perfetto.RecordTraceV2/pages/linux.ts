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

import type {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import type {TraceConfigBuilder} from '../config/trace_config_builder';
import {Dropdown} from './widgets/dropdown';
import {Textarea} from './widgets/textarea';

const JOURNALD_DS = 'linux.systemd_journald';

const MIN_PRIO_OPTIONS = [
  {value: 0, label: 'Emergency'},
  {value: 1, label: 'Alert'},
  {value: 2, label: 'Critical'},
  {value: 3, label: 'Error'},
  {value: 4, label: 'Warning'},
  {value: 5, label: 'Notice'},
  {value: 6, label: 'Info'},
  {value: 7, label: 'Debug'},
] as const;

export function linuxRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'linux',
    title: 'Linux',
    subtitle: 'Linux-specific data sources',
    icon: 'dns',
    probes: [journald()],
  };
}

function journald(): RecordProbe {
  const settings = {
    minPrio: new Dropdown<number>({
      title: 'Minimum syslog priority',
      options: MIN_PRIO_OPTIONS,
      defaultValue: 7,
    }),
    identifiers: new Textarea({
      title: 'Process name (SYSLOG_IDENTIFIER) to record',
      placeholder: 'One identifier per line\nsshd\nmy-service',
    }),
    units: new Textarea({
      title: 'systemd units to record',
      placeholder:
        'One unit per line\nsystemd-journald.service\nuser@1000.service',
    }),
  };

  return {
    id: 'systemd_journald',
    title: 'Journald log messages',
    description: 'Records log messages from systemd-journald.',
    supportedPlatforms: ['LINUX'],
    settings,
    genConfig(tc: TraceConfigBuilder) {
      const ds = tc.addDataSource(JOURNALD_DS);
      const journaldConfig = (ds.journaldConfig ??= {});
      journaldConfig.minPrio = settings.minPrio.value;

      const identifiers = settings.identifiers.text
        .split('\n')
        .map((line) => line.trim())
        .filter((line) => line.length > 0);
      if (identifiers.length > 0) {
        journaldConfig.filterIdentifiers = identifiers;
      }
      const units = settings.units.text
        .split('\n')
        .map((line) => line.trim())
        .filter((line) => line.length > 0);
      if (units.length > 0) {
        journaldConfig.filterUnits = units;
      }
    },
  };
}
