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

import protos from '../../../protos';
import {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {POLL_INTERVAL_SLIDER, Slider} from './widgets/slider';

export function powerRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'power',
    title: 'Power',
    subtitle: 'Battery and other energy counters',
    icon: 'battery_charging_full',
    probes: [powerRails(), powerVoltages()],
  };
}

function powerRails(): RecordProbe {
  const ANDROID_POWER_DS = 'android.power';
  const settings = {pollMs: new Slider(POLL_INTERVAL_SLIDER)};
  return {
    id: 'power_rails',
    image: 'rec_battery_counters.png',
    title: 'Battery drain & power rails',
    description:
      'Polls charge counters and instantaneous power draw from ' +
      'the battery power management IC and the power rails from ' +
      'the PowerStats HAL.',
    docsLink: 'https://perfetto.dev/docs/data-sources/battery-counters',
    supportedPlatforms: ['ANDROID'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addDataSource(ANDROID_POWER_DS).androidPowerConfig = {
        batteryPollMs: settings.pollMs.value,
        collectPowerRails: true,
        batteryCounters: [
          protos.AndroidPowerConfig.BatteryCounters
            .BATTERY_COUNTER_CAPACITY_PERCENT,
          protos.AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CHARGE,
          protos.AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CURRENT,
        ],
      };
    },
  };
}

function powerVoltages(): RecordProbe {
  return {
    id: 'power_voltages',
    image: 'rec_board_voltage.png',
    title: 'Board voltages & frequencies',
    description: 'Tracks voltage and frequency changes from board sensors',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(
        'regulator/regulator_set_voltage',
        'regulator/regulator_set_voltage_complete',
        'power/clock_enable',
        'power/clock_disable',
        'power/clock_set_rate',
        'power/suspend_resume',
      );
    },
  };
}
