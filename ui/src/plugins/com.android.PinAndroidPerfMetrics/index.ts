import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {METRIC_HANDLERS} from './handlers/handlerRegistry';
import type {MetricData, MetricHandlerMatch} from './handlers/metricUtils';
import AndroidCujsPlugin from '../com.android.AndroidCujs';
import Wattson from '../org.kernel.Wattson';
import {NUM} from '../../trace_processor/query_result';

const JANK_CUJ_QUERY_PRECONDITIONS = `
  SELECT RUN_METRIC('android/android_blocking_calls_cuj_metric.sql');
`;

function getParamFromHash(paramName: string): string | undefined {
  const hash = location.hash;
  const regex = new RegExp(`dev.perfetto.PinAndroidPerfMetrics:${paramName}=([^&]*)`);
  const match = hash.match(regex);
  if (match === null) {
    return undefined;
  }
  return decodeURIComponent(match[1]);
}

function getMetricsFromHash(): string[] {
  // TODO(stevegolton): this uses `dev.perfetto.PinAndroidPerfMetrics` for
  // back-compat reasons only. Figure out a way to preserve backwards
  // compatibility of plugin arguments when plugins change id.
  const capturedString = getParamFromHash('metrics');
  if (capturedString === undefined) {
    return [];
  }
  let metricList: string[] = [];
  if (capturedString.includes('--')) {
    metricList = capturedString.split('--');
  } else {
    metricList = [capturedString];
  }
  return metricList;
}

let metrics: string[];

/**
 * Plugin that adds and pins the debug track for the metric passed
 * For more context -
 * This plugin reads the names of regressed metrics from the url upon loading
 * It then checks the metric names against some handlers and if they
 * match it accordingly adds the debug tracks for them
 * This way when comparing two different perfetto traces before and after
 * the regression, the user will not have to manually search for the
 * slices related to the regressed metric
 */
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.PinAndroidPerfMetrics';
  static readonly dependencies = [AndroidCujsPlugin, Wattson];

  static onActivate(): void {
    metrics = getMetricsFromHash();
    Wattson.updateWindowsOfInterest(metrics);
  }

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'com.android.PinAndroidPerfMetrics',
      name: 'Add and Pin: Jank Metric Slice',
      callback: async () => {
        const metric = await ctx.omnibox.prompt(
          'Metrics names (separated by comma)',
        );
        if (metric === undefined) return;
        const metricList = metric.split(',');
        this.callHandlers(metricList, ctx);
      },
    });
    if (metrics.length !== 0) {
      const plugin = ctx.plugins.getPlugin(AndroidCujsPlugin);
      await plugin.pinJankCujs(ctx);
      await plugin.pinLatencyCujs(ctx);
      this.callHandlers(metrics, ctx);
    }

    const cujName = getParamFromHash('cuj');
    const cujId = getParamFromHash('cuj_id');
    if (cujName !== undefined || cujId !== undefined) {
      await this.pinAndPanToCuj(ctx, cujName, cujId);
    }
  }

  private async pinAndPanToCuj(ctx: Trace, cujName?: string, cujId?: string) {
    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);

    let query = '';
    if (cujId !== undefined) {
      query = `SELECT slice_id FROM android_jank_cuj WHERE cuj_id = ${cujId} LIMIT 1`;
    } else if (cujName !== undefined) {
      query = `SELECT slice_id FROM android_jank_cuj WHERE cuj_name = '${cujName}' OR cuj_slice_name = '${cujName}' LIMIT 1`;
    } else {
      return;
    }

    const result = await ctx.engine.query(query);
    if (result.numRows() === 0) {
      return;
    }
    const sliceId = result.firstRow({slice_id: NUM}).slice_id;

    ctx.selection.selectSqlEvent('slice', sliceId, {scrollToSelection: true});
  }

  private async callHandlers(metricsList: string[], ctx: Trace) {
    // List of metrics that actually match some handler
    const metricsToShow: MetricHandlerMatch[] =
      this.getMetricsToShow(metricsList);

    if (metricsToShow.length === 0) {
      return;
    }

    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
    for (const {metricData, metricHandler} of metricsToShow) {
      metricHandler.addMetricTrack(metricData, ctx);
    }
  }

  private getMetricsToShow(metricList: string[]): MetricHandlerMatch[] {
    const sortedMetricList = [...metricList].sort();
    const validMetrics: MetricHandlerMatch[] = [];
    const alreadyMatchedMetricData: Set<string> = new Set();
    for (const metric of sortedMetricList) {
      for (const metricHandler of METRIC_HANDLERS) {
        const metricData = metricHandler.match(metric);
        if (!metricData) continue;
        const jsonMetricData = this.metricDataToJson(metricData);
        if (!alreadyMatchedMetricData.has(jsonMetricData)) {
          alreadyMatchedMetricData.add(jsonMetricData);
          validMetrics.push({
            metricData: metricData,
            metricHandler: metricHandler,
          });
        }
      }
    }
    return validMetrics;
  }

  private metricDataToJson(metricData: MetricData): string {
    // Used to have a deterministic keys order.
    return JSON.stringify(metricData, Object.keys(metricData).sort());
  }
}
