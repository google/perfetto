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
import {Combobox} from '../../../widgets/combobox';
import {renderWidgetShowcase} from '../widgets_page_utils';

const SAMPLE_SUGGESTIONS = [
  'slice',
  'sched_slice',
  'thread_state',
  'thread',
  'process',
  'counter',
  'android_logs',
  'cpu_counter_track',
  'gpu_slice',
  'gpu_track',
  'gpu_counter_track',
  'gpu_counter_group',
  'heap_graph_object',
  'heap_graph_class',
  'heap_graph_reference',
  'heap_profile_allocation',
  'perf_sample',
  'stack_profile_frame',
  'stack_profile_callsite',
  'stack_profile_mapping',
  'profiler_smaps',
  'trace_stats',
  'metadata',
  'args',
  'raw',
  'ftrace_event',
  'flow',
  'span_join',
  'window',
  'experimental_slice_layout',
  'experimental_annotated_callstack',
  'experimental_flat_slice',
  'android_battery_stats',
  'android_network_packets',
  'surfaceflinger_layers_snapshot',
  'surfaceflinger_transactions',
  'v8_isolate',
  'v8_js_script',
  'v8_js_function',
  'v8_js_code',
  'v8_internal_code',
  'v8_wasm_code',
  'v8_regexp_code',
  'memory_snapshot',
  'memory_snapshot_node',
  'memory_snapshot_edge',
];

let selectedValue = '';

export function renderCombobox(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Combobox'),
      m(
        'p',
        'A text input with fuzzy-filtered suggestions. Type to narrow the list, ' +
          'use arrow keys to navigate, and press Enter or click to accept. ' +
          'The typed value is always preserved — suggestions are non-binding.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: ({icon, itemIcons, ...opts}) =>
        m(Combobox, {
          ...opts,
          icon: icon ? 'table_chart' : undefined,
          value: selectedValue,
          suggestions: itemIcons
            ? SAMPLE_SUGGESTIONS.map((s) => ({value: s, icon: 'table_chart'}))
            : SAMPLE_SUGGESTIONS,
          placeholder: 'Pick a table name...',
          onChange: (value: string) => {
            selectedValue = value;
          },
        }),
      initialOpts: {
        icon: true,
        itemIcons: false,
      },
    }),
  ];
}
