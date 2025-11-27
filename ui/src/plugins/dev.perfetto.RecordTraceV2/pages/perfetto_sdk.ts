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

import {splitLinesNonEmpty} from '../../../base/string_utils';
import {RecordSubpage, RecordProbe} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {TypedMultiselect} from './widgets/multiselect';
import {Textarea} from './widgets/textarea';

export function perfettoSDKRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'track_event',
    title: 'Perfetto SDK',
    subtitle: 'Perfetto Tracing SDK annotations',
    icon: 'speed',
    probes: [trackEvent()],
  };
}

function trackEvent(): RecordProbe {
  const settings = {
    categories: new TypedMultiselect<string>({
      options: new Map(
        Object.entries(TRACK_EVENT_CATEGORIES).map(([id, name]) => [
          `${id}: ${name}`,
          id,
        ]),
      ),
    }),
    enabledCats: new Textarea({
      title: 'Additional categories:',
      placeholder: 'e.g. cat1\ncat2_*',
    }),
  };
  return {
    id: 'track_event',
    title: 'Track events',
    image: 'rec_atrace.png',
    description:
      'Enables C / C++ / Java annotations (PERFETTO_TE_SLICE_BEGIN(), TRACE_EVENT(), os.PerfettoTrace())',
    supportedPlatforms: ['ANDROID', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addTrackEventDisabledCategories('*');
      tc.addTrackEventEnabledCategories(
        ...settings.categories.selectedValues(),
      );
      for (const line of splitLinesNonEmpty(settings.enabledCats.text)) {
        tc.addTrackEventEnabledCategories(line);
      }
    },
  };
}

// TODO: query all categories from the device and concat everything together.
const TRACK_EVENT_CATEGORIES = {
  mq: 'Message Queue',
  gfx: 'Graphics',
  servicemanager: 'Service Manager',
};
