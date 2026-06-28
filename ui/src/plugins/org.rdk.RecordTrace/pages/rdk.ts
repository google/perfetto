// Copyright 2026 Comcast Cable Communications Management, LLC
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
import {RecordSubpage, RecordProbe} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {TypedMultiselect} from './widgets/multiselect';

export function rdkRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'rdk',
    title: 'RDK apps & svcs',
    subtitle: 'RDK-specific data sources',
    icon: 'tv',
    probes: [journal()],
  };
}

function journal(): RecordProbe {
  const settings = {
    buffers: new TypedMultiselect<protos.AndroidLogId>({
      options: new Map(
        Object.entries({
          Apps: protos.AndroidLogId.LID_DEFAULT,
          Kernel: protos.AndroidLogId.LID_KERNEL,
          System: protos.AndroidLogId.LID_SYSTEM,
        }),
      ),
      defaultSelected: ['Apps', 'System'],
    }),
  };
  return {
    id: 'journal',
    title: 'Logging (journal)',
    image: 'rec_logcat.png',
    description:
      'Streams the journal log into the trace. This contains most ' +
      'app logs, as well as other AS components.',
    supportedPlatforms: ['LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const logIds = settings.buffers.selectedValues();
      tc.addDataSource('rdk.journal').androidLogConfig = {
        logIds: logIds.length > 0 ? logIds : undefined,
      };
    },
  };
}
