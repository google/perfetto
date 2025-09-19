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
import {Engine} from '../../trace_processor/engine';
import {NUM, STR_NULL} from '../../trace_processor/query_result';

enum FtraceEvent {
  CPU_FREQUENCY = 'power/cpu_frequency',
  CPU_IDLE = 'power/cpu_idle',
  DEVFREQ_FREQUENCY = 'devfreq/devfreq_frequency',
  CPUHP_ENTER = 'cpuhp/cpuhp_enter',
  CPUHP_EXIT = 'cpuhp/cpuhp_exit',
  CPUHP_MULTI_ENTER = 'cpuhp/cpuhp_multi_enter',
  PRINT = 'ftrace/print',
  SUSPEND_RESUME = 'power/suspend_resume',
  SCHED_SWITCH = 'sched/sched_switch',
}

// Walk through user's Perfetto Trace Configs and check
// against bare minimum configs that makes Wattson work.
// Add the missing ones to missingEvents, display in UI.
export async function hasWattsonSufficientCPUConfigs(
  engine: Engine,
): Promise<string[]> {
  // 1. Determine required events first
  const requiredEvents = new Set<FtraceEvent>([
    FtraceEvent.CPU_FREQUENCY,
    FtraceEvent.CPU_IDLE,
  ]);

  const dsuDependencyQuery = await engine.query(
    `
    INCLUDE PERFETTO MODULE wattson.curves.utils;
    SELECT count(*) AS count FROM _cpu_w_dsu_dependency;
    `,
  );

  if (dsuDependencyQuery.firstRow({count: NUM}).count > 0) {
    requiredEvents.add(FtraceEvent.DEVFREQ_FREQUENCY);
  }

  // 2. Check configured events
  const query = `
    SELECT str_value
    FROM metadata
    WHERE name = 'trace_config_pbtxt';
    `;

  const result = await engine.query(query);
  const row = result.maybeFirstRow({str_value: STR_NULL});
  const traceConfig = row?.str_value || '';

  const foundEvents = new Set<FtraceEvent>();

  if (/cpufreq_period_ms:\s*\d+/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.CPU_FREQUENCY);
  }

  if (/cpuidle_period_ms:\s*\d+/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.CPU_IDLE);
  }

  // below events are included in "freq" Atrace category.
  if (/atrace_categories:\s*"freq"/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.CPU_FREQUENCY);
    foundEvents.add(FtraceEvent.DEVFREQ_FREQUENCY);
    foundEvents.add(FtraceEvent.SUSPEND_RESUME);
    foundEvents.add(FtraceEvent.CPUHP_ENTER);
    foundEvents.add(FtraceEvent.CPUHP_EXIT);
  }

  // below events are included in "idle" Atrace category.
  if (/atrace_categories:\s*"idle"/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.CPU_IDLE);
  }

  // below events are included in "sched" Atrace category.
  if (/atrace_categories:\s*"sched"/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.SCHED_SWITCH);
  }

  for (const event of Object.values(FtraceEvent)) {
    const eventPattern = new RegExp(`ftrace_events:\\s*"${event}"`);
    if (eventPattern.test(traceConfig)) {
      foundEvents.add(event);
    }
  }

  // 3. Compare required events with found events
  const missingEvents: string[] = [];
  for (const requiredEvent of requiredEvents) {
    if (!foundEvents.has(requiredEvent)) {
      missingEvents.push(requiredEvent);
    }
  }

  return missingEvents;
}

export function getWattsonCpuWarning(missingEvents: string[]): m.Children {
  if (missingEvents.length === 0) return null;
  return m(
    '.pf-wattson-warning',
    'Perfetto trace configuration is missing below trace_events for Wattson to work:',
    m(
      '.pf-wattson-warning__list',
      missingEvents.map((event) => m('li', event)),
    ),
  );
}
