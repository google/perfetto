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

import {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {Toggle} from './widgets/toggle';
import {Textarea} from './widgets/textarea';
import {splitLinesNonEmpty} from '../../../base/string_utils';
import {Slider} from './widgets/slider';

export function networkRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'network',
    title: 'Network',
    subtitle: 'Network activity, Wi-Fi events',
    icon: 'wifi',
    probes: [wifiNetworkTracing()],
  };
}

function wifiNetworkTracing(): RecordProbe {
  const cfgMacEvents = ['cfg80211/*', 'mac80211/*'];
  const netEvents = ['net/netif_receive_skb', 'net/net_dev_xmit'];
  const settings = {
    cfg_mac: new Toggle({
      title: '802.11 layer events',
      cssClass: '.thin',
      default: false,
      descr: 'Configuration (cfg80211) and MAC layer events (mac80211).',
    }),
    net: new Toggle({
      title: 'Packets TX/RX',
      cssClass: '.thin',
      default: false,
      descr: 'Kernel events for packet transmission/reception.',
    }),
    bufSizeMb: new Slider({
      title: 'Dedicated buffer for packet tracing (MB)',
      cssClass: '.thin',
      values: [0, 4, 8, 16, 32, 64, 128, 256, 512],
      unit: 'MB',
      zeroIsDefault: true,
    }),
    driverEventsText: new Textarea({
      title:
        'Additional ftrace events (e.g. driver specific ones). ' +
        'Format is "family_name/event_name".',
      placeholder: 'One per line',
    }),
  };
  return {
    id: 'wifi_network_tracing',
    image: 'rec_wifi.png',
    title: 'Wi-Fi/network ftrace events',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    description:
      'Tracing of kernel events related to Wi-Fi/network operation.\n' +
      'Note - these events are meant only to facilitate debugging of ' +
      'communication problems and have potential to overflow your buffers ' +
      'leading to data loss.',
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      if (settings.cfg_mac.enabled) {
        tc.addFtraceEvents(...cfgMacEvents);
      }
      if (settings.net.enabled) {
        if (settings.bufSizeMb.value === 0) {
          tc.addFtraceEvents(...netEvents);
        } else {
          const bufId = 'ftrace_net';
          tc.addBuffer(bufId, settings.bufSizeMb.value * 1024);
          const cfg = tc.addDataSource('linux.ftrace', bufId);
          cfg.ftraceConfig ??= {};
          cfg.ftraceConfig.ftraceEvents ??= [];
          cfg.ftraceConfig.ftraceEvents.push(...netEvents);
        }
      }
      if (settings.driverEventsText.text) {
        const [driverEvents] = extractEvents(settings.driverEventsText.text);
        tc.addFtraceEvents(...driverEvents);
      }
    },
  };
}

function extractEvents(text: string): [string[]] {
  const events = [];
  for (const line of splitLinesNonEmpty(text)) {
    events.push(line);
  }
  return [events];
}
