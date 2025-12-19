import m, {Vnode} from 'mithril';
import {Engine} from '../../trace_processor/engine';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {linkify} from '../../widgets/anchor';

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
  const requiredEvents = new Set<FtraceEvent>([FtraceEvent.CPU_FREQUENCY]);

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

  // below events are included in "freq" Atrace category.
  if (/atrace_categories:\s*"freq"/.test(traceConfig)) {
    foundEvents.add(FtraceEvent.CPU_FREQUENCY);
    foundEvents.add(FtraceEvent.DEVFREQ_FREQUENCY);
    foundEvents.add(FtraceEvent.SUSPEND_RESUME);
    foundEvents.add(FtraceEvent.CPUHP_ENTER);
    foundEvents.add(FtraceEvent.CPUHP_EXIT);
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

export function createCpuWarnings(
  missingEvents: string[],
  realCpuIdleCounters: boolean,
): Vnode | undefined {
  const warningMsg: Vnode[] = [];

  if (missingEvents.length > 0) {
    warningMsg.push(
      m(
        '.pf-wattson-warning',
        linkify(
          `See https://source.android.com/docs/core/power/wattson/how-to-wattson for more details on Wattson's required trace configuration. The following ftrace_events are necessary for Wattson to make power estimates:`,
        ),
        m(
          '.pf-wattson-warning__list',
          missingEvents.map((event) => m('li', event)),
        ),
      ),
    );
  }

  if (!realCpuIdleCounters) {
    if (warningMsg.length > 0) {
      warningMsg.push(m('hr'));
    }
    warningMsg.push(
      m(
        'p',
        '`cpu_idle` counters are not available in this trace; deriving cpu_idle counters from the swapper thread.',
      ),
    );
  }

  return warningMsg.length > 0
    ? m('.pf-wattson-warning', warningMsg)
    : undefined;
}
