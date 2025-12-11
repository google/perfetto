// Copyright (C) 2023 The Android Open Source Project
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
import z from 'zod';
import {searchSegment} from '../../base/binary_search';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {assertTrue, assertUnreachable} from '../../base/logging';
import {Time, time} from '../../base/time';
import {uuidv4Sql} from '../../base/uuid';
import {raf} from '../../core/raf_scheduler';
import {Trace} from '../../public/trace';
import {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import {checkerboardExcept} from '../checkerboard';
import {CacheKey} from './timeline_cache';
import {valueIfAllEqual} from '../../base/array_utils';

function roundAway(n: number): number {
  const exp = Math.ceil(Math.log10(Math.max(Math.abs(n), 1)));
  const pow10 = Math.pow(10, exp);
  return Math.sign(n) * (Math.ceil(Math.abs(n) / (pow10 / 20)) * (pow10 / 20));
}

function toLabel(n: number): string {
  if (n === 0) {
    return '0';
  }
  const units: [number, string][] = [
    [0.000000001, 'n'],
    [0.000001, 'u'],
    [0.001, 'm'],
    [1, ''],
    [1000, 'K'],
    [1000 * 1000, 'M'],
    [1000 * 1000 * 1000, 'G'],
    [1000 * 1000 * 1000 * 1000, 'T'],
  ];
  let largestMultiplier;
  let largestUnit;
  [largestMultiplier, largestUnit] = units[0];
  const absN = Math.abs(n);
  for (const [multiplier, unit] of units) {
    if (multiplier > absN) {
      break;
    }
    [largestMultiplier, largestUnit] = [multiplier, unit];
  }
  return `${Math.round(n / largestMultiplier)}${largestUnit}`;
}

class RangeSharer {
  private static traceToRangeSharer = new WeakMap<Trace, RangeSharer>();

  private tagToRange: Map<string, [number, number]>;
  private keyToEnabled: Map<string, boolean>;

  constructor() {
    this.tagToRange = new Map();
    this.keyToEnabled = new Map();
  }

  static getRangeSharer(trace: Trace): RangeSharer {
    let sharer = RangeSharer.traceToRangeSharer.get(trace);
    if (sharer === undefined) {
      sharer = new RangeSharer();
      RangeSharer.traceToRangeSharer.set(trace, sharer);
    }
    return sharer;
  }

  isEnabled(key: string): boolean {
    const value = this.keyToEnabled.get(key);
    if (value === undefined) {
      return true;
    }
    return value;
  }

  setEnabled(key: string, enabled: boolean): void {
    this.keyToEnabled.set(key, enabled);
  }

  share(
    options: CounterOptions,
    [min, max]: [number, number],
  ): [number, number] {
    const key = options.yRangeSharingKey;
    if (key === undefined || !this.isEnabled(key)) {
      return [min, max];
    }

    const tag = `${options.yRangeSharingKey}-${options.yMode}-${
      options.yDisplay
    }-${options.chartHeightSize}`;
    const cachedRange = this.tagToRange.get(tag);
    if (cachedRange === undefined) {
      this.tagToRange.set(tag, [min, max]);
      return [min, max];
    }

    cachedRange[0] = Math.min(min, cachedRange[0]);
    cachedRange[1] = Math.max(max, cachedRange[1]);

    return [cachedRange[0], cachedRange[1]];
  }
}

interface CounterData {
  timestamps: BigInt64Array;
  minDisplayValues: Float64Array;
  maxDisplayValues: Float64Array;
  lastDisplayValues: Float64Array;
  displayValueRange: [number, number];
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 3.5;

interface CounterLimits {
  maxDisplayValue: number;
  minDisplayValue: number;
}

interface CounterTooltipState {
  lastDisplayValue: number;
  ts: time;
  tsEnd?: time;
}

type ChartHeightSize = 1 | 2 | 4 | 8 | 16 | 32;

const CHART_HEIGHT_LABELS: [string, ChartHeightSize][] = [
  ['Small (1x)', 1],
  ['Medium (2x)', 2],
  ['Large (4x)', 4],
  ['XLarge (8x)', 8],
  ['XXLarge (16x)', 16],
  ['XXXLarge (32x)', 32],
];

export interface CounterOptions {
  // Mode for computing the y value. Options are:
  // value = v[t] directly the value of the counter at time t
  // delta = v[t] - v[t-1] delta between value and previous value
  // rate = (v[t] - v[t-1]) / dt as delta but normalized for time
  yMode: 'value' | 'delta' | 'rate';

  // Whether Y scale should cover all of the possible values (and therefore, be
  // static) or whether it should be dynamic and cover only the visible values.
  yRange: 'all' | 'viewport';

  // Whether the Y scale should:
  // zero = y-axis scale should cover the origin (zero)
  // minmax = y-axis scale should cover just the range of yRange
  // log = as minmax but also use a log scale
  yDisplay: 'zero' | 'minmax' | 'log';

  // Whether the range boundaries should be strict and use the precise min/max
  // values or whether they should be rounded down/up to the nearest human
  // readable value.
  yRangeRounding: 'strict' | 'human_readable';

  // Scales the height of the chart.
  chartHeightSize: ChartHeightSize;

  // Allows *extending* the range of the y-axis counter increasing
  // the maximum (via yOverrideMaximum) or decreasing the minimum
  // (via yOverrideMinimum). This is useful for percentage counters
  // where the range (0-100) is known statically upfront and even if
  // the trace only includes smaller values.
  yOverrideMaximum?: number;
  yOverrideMinimum?: number;

  // If set all counters with the same key share a range.
  yRangeSharingKey?: string;

  // unit for the counter. This is displayed in the tooltip and
  // legend.
  unit?: string;

  // unit to use when yMode is set to 'rate'. This rateUnit should be
  // equivalent to unit/s. For example, if the 'unit' is Joules, the 'rateUnit'
  // may be set to Watts. If not specified, unit/s will be used.
  rateUnit?: string;
}

const radioIconChecked = 'radio_button_checked';
const radioIconUnchecked = 'radio_button_unchecked';

const ymodeSchema = z.enum(['value', 'delta', 'rate']);
type yMode = z.infer<typeof ymodeSchema>;

const yRangeSchema = z.union([z.literal('all'), z.literal('viewport')]);
type YRange = z.infer<typeof yRangeSchema>;

const yDisplaySchema = z.enum(['zero', 'minmax', 'log']);
type YDisplay = z.infer<typeof yDisplaySchema>;

const yRangeRoundingSchema = z.union([
  z.literal('strict'),
  z.literal('human_readable'),
]);
type YRangeRounding = z.infer<typeof yRangeRoundingSchema>;

const chartHeightSizeSchema = z.union([
  z.literal(1),
  z.literal(2),
  z.literal(4),
  z.literal(8),
  z.literal(16),
  z.literal(32),
]);

const yModeSettingDescriptor: TrackSettingDescriptor<yMode> = {
  id: 'yMode',
  name: 'Mode',
  description: 'value, delta, rate',
  schema: ymodeSchema,
  defaultValue: 'value',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Mode (currently: ${value ?? 'mixed'})`}, [
      m(MenuItem, {
        label: 'Value',
        onclick: () => setter('value'),
        icon: value === 'value' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Delta',
        onclick: () => setter('delta'),
        icon: value === 'delta' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Rate',
        onclick: () => setter('rate'),
        icon: value === 'rate' ? radioIconChecked : radioIconUnchecked,
      }),
    ]);
  },
};

const yRangeSettingDescriptor: TrackSettingDescriptor<YRange> = {
  id: 'yRange',
  name: 'Y-axis range',
  description: 'all, viewport',
  schema: yRangeSchema,
  defaultValue: 'all',
  render(setter, values) {
    const value = valueIfAllEqual(values);

    const icon = (() => {
      switch (value) {
        case 'viewport':
          return 'check_box';
        case 'all':
          return 'check_box_outline_blank';
        default:
          return 'indeterminate_check_box';
      }
    })();

    return m(MenuItem, {
      label: 'Zoom on scroll',
      icon,
      onclick: () => {
        switch (value) {
          case 'all':
            setter('viewport');
            break;
          case 'viewport':
          default:
            setter('all');
            break;
        }
      },
    });
  },
};

const yDisplaySettingDescriptor: TrackSettingDescriptor<YDisplay> = {
  id: 'yDisplay',
  name: 'Y-axis display',
  description: 'zero, minmax, log',
  schema: yDisplaySchema,
  defaultValue: 'zero',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Display (currently: ${value ?? 'mixed'})`}, [
      m(MenuItem, {
        label: 'Zero-based',
        onclick: () => setter('zero'),
        icon: value === 'zero' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Min/Max',
        onclick: () => setter('minmax'),
        icon: value === 'minmax' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Log',
        onclick: () => setter('log'),
        icon: value === 'log' ? radioIconChecked : radioIconUnchecked,
      }),
    ]);
  },
};

const yRangeRoundingSettingDescriptor: TrackSettingDescriptor<YRangeRounding> =
  {
    id: 'yRangeRounding',
    name: 'Y-axis rounding',
    description: 'strict, human_readable',
    schema: yRangeRoundingSchema,
    defaultValue: 'human_readable',
    render(setter, values) {
      const value = valueIfAllEqual(values);

      const icon = (() => {
        switch (value) {
          case 'human_readable':
            return 'check_box';
          case 'strict':
            return 'check_box_outline_blank';
          default:
            return 'indeterminate_check_box';
        }
      })();

      return m(MenuItem, {
        label: 'Round y-axis scale',
        icon,
        onclick: () => {
          switch (value) {
            case 'human_readable':
              setter('strict');
              break;
            case 'strict':
            default:
              setter('human_readable');
              break;
          }
        },
      });
    },
  };

const chartHeightSizeSettingDescriptor: TrackSettingDescriptor<ChartHeightSize> =
  {
    id: 'chartHeightSize',
    name: 'Chart height',
    description: '1, 4, 8, 16, 32',
    schema: chartHeightSizeSchema,
    defaultValue: 1,
    render(setter, values) {
      const value = valueIfAllEqual(values);
      return m(MenuItem, {label: `Enlarge (currently: ${value ?? 'mixed'})`}, [
        CHART_HEIGHT_LABELS.map(([label, size]) =>
          m(MenuItem, {
            label,
            onclick: () => setter(size),
            icon: value === size ? radioIconChecked : radioIconUnchecked,
          }),
        ),
      ]);
    },
  };

export abstract class BaseCounterTrack implements TrackRenderer {
  protected trackUuid = uuidv4Sql();

  // This is the over-skirted cached bounds:
  private countersKey: CacheKey = CacheKey.zero();

  private counters: CounterData = {
    timestamps: new BigInt64Array(0),
    minDisplayValues: new Float64Array(0),
    maxDisplayValues: new Float64Array(0),
    lastDisplayValues: new Float64Array(0),
    displayValueRange: [0, 0],
  };

  private limits?: CounterLimits;

  private hover?: CounterTooltipState;
  private options?: CounterOptions;
  private readonly rangeSharer: RangeSharer;

  private readonly trash: AsyncDisposableStack;

  private getCounterOptions(): CounterOptions {
    if (this.options === undefined) {
      const options = this.getDefaultCounterOptions();
      for (const [key, value] of Object.entries(this.defaultOptions)) {
        if (value !== undefined) {
          // eslint-disable-next-line @typescript-eslint/no-explicit-any
          (options as any)[key] = value;
        }
      }
      this.options = options;
    }
    return this.options;
  }

  renderTooltip(): m.Children {
    if (this.hover) {
      const value =
        this.options?.yDisplay === 'log'
          ? Math.exp(this.hover.lastDisplayValue)
          : this.hover.lastDisplayValue;

      return m('.pf-track__tooltip', this.formatValue(value));
    } else {
      return undefined;
    }
  }

  private formatValue(value: number) {
    const options = this.getCounterOptions();
    const unit = this.unit;
    switch (options.yMode) {
      case 'value':
        return `${value.toLocaleString()} ${unit}`;
      case 'delta':
        return `${value.toLocaleString()} \u0394${unit}`;
      case 'rate':
        return `${value.toLocaleString()} ${this.rateUnit}`;
      default:
        assertUnreachable(options.yMode);
    }
  }

  // Extension points.

  // onInit hook lets you do asynchronous set up e.g. creating a table
  // etc. We guarantee that this will be resolved before doing any
  // queries using the result of getSqlSource(). All persistent
  // state in trace_processor should be cleaned up when dispose is
  // called on the returned hook.
  async onInit(): Promise<AsyncDisposable | void> {}

  // This should be an SQL expression returning the columns `ts` and `value`.
  abstract getSqlSource(): string;

  protected getDefaultCounterOptions(): CounterOptions {
    return {
      yRange: 'all',
      yRangeRounding: 'human_readable',
      yMode: 'value',
      yDisplay: 'zero',
      chartHeightSize: 1,
    };
  }

  constructor(
    protected readonly trace: Trace,
    protected readonly uri: string,
    protected readonly defaultOptions: Partial<CounterOptions> = {},
  ) {
    this.trash = new AsyncDisposableStack();
    this.rangeSharer = RangeSharer.getRangeSharer(trace);
  }

  getHeight() {
    const height = 40;
    return height * this.getCounterOptions().chartHeightSize;
  }

  // A method to render menu items for switching the defualt
  // rendering options.  Useful if a subclass wants to incorporate it
  // as a submenu.
  protected getCounterContextMenuItems(): m.Children {
    const options = this.getCounterOptions();

    return [
      m(
        MenuItem,
        {
          label: `Display (currently: ${options.yDisplay})`,
        },

        m(MenuItem, {
          label: 'Zero-based',
          icon:
            options.yDisplay === 'zero'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yDisplay = 'zero';
            this.invalidate();
          },
        }),

        m(MenuItem, {
          label: 'Min/Max',
          icon:
            options.yDisplay === 'minmax'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yDisplay = 'minmax';
            this.invalidate();
          },
        }),

        m(MenuItem, {
          label: 'Log',
          icon:
            options.yDisplay === 'log'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yDisplay = 'log';
            this.invalidate();
          },
        }),
      ),

      m(
        MenuItem,
        {
          label: `Enlarge (currently: ${options.chartHeightSize}x)`,
        },
        CHART_HEIGHT_LABELS.map(([label, size]) =>
          m(MenuItem, {
            label,
            icon:
              options.chartHeightSize === size
                ? 'radio_button_checked'
                : 'radio_button_unchecked',
            onclick: () => {
              options.chartHeightSize = size;
              this.invalidate();
            },
          }),
        ),
      ),

      m(MenuItem, {
        label: 'Zoom on scroll',
        icon:
          options.yRange === 'viewport'
            ? 'check_box'
            : 'check_box_outline_blank',
        onclick: () => {
          options.yRange = options.yRange === 'viewport' ? 'all' : 'viewport';
          this.invalidate();
        },
      }),

      options.yRangeSharingKey &&
        m(MenuItem, {
          label: `Share y-axis scale (group: ${options.yRangeSharingKey})`,
          icon: this.rangeSharer.isEnabled(options.yRangeSharingKey)
            ? 'check_box'
            : 'check_box_outline_blank',
          onclick: () => {
            const key = options.yRangeSharingKey;
            if (key === undefined) {
              return;
            }
            this.rangeSharer.setEnabled(key, !this.rangeSharer.isEnabled(key));
            this.invalidate();
          },
        }),

      m(MenuDivider),
      m(
        MenuItem,
        {
          label: `Mode (currently: ${options.yMode})`,
        },

        m(MenuItem, {
          label: 'Value',
          icon:
            options.yMode === 'value'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yMode = 'value';
            this.invalidate();
          },
        }),

        m(MenuItem, {
          label: 'Delta',
          icon:
            options.yMode === 'delta'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yMode = 'delta';
            this.invalidate();
          },
        }),

        m(MenuItem, {
          label: 'Rate',
          icon:
            options.yMode === 'rate'
              ? 'radio_button_checked'
              : 'radio_button_unchecked',
          onclick: () => {
            options.yMode = 'rate';
            this.invalidate();
          },
        }),
      ),
      m(MenuItem, {
        label: 'Round y-axis scale',
        icon:
          options.yRangeRounding === 'human_readable'
            ? 'check_box'
            : 'check_box_outline_blank',
        onclick: () => {
          options.yRangeRounding =
            options.yRangeRounding === 'human_readable'
              ? 'strict'
              : 'human_readable';
          this.invalidate();
        },
      }),
    ];
  }

  protected invalidate() {
    this.limits = undefined;
    this.countersKey = CacheKey.zero();
    this.counters = {
      timestamps: new BigInt64Array(0),
      minDisplayValues: new Float64Array(0),
      maxDisplayValues: new Float64Array(0),
      lastDisplayValues: new Float64Array(0),
      displayValueRange: [0, 0],
    };
    this.hover = undefined;

    raf.scheduleFullRedraw();
  }

  // A method to render a context menu corresponding to switching the rendering
  // modes. By default, getTrackShellButtons renders it, but a subclass can call
  // it manually, if they want to customise rendering track buttons.
  protected getCounterContextMenu(): m.Child {
    return m(
      PopupMenu,
      {
        trigger: m(Button, {icon: 'show_chart', compact: true}),
      },
      this.getCounterContextMenuItems(),
    );
  }

  getTrackShellButtons(): m.Children {
    return this.getCounterContextMenu();
  }

  async onCreate(): Promise<void> {
    const result = await this.onInit();
    result && this.trash.use(result);
    this.limits = await this.createTableAndFetchLimits(false);
  }

  readonly yModeSetting: TrackSetting<yMode> = {
    descriptor: yModeSettingDescriptor,
    getValue: () => this.getCounterOptions().yMode,
    setValue: (yMode) => {
      this.options = {...this.getCounterOptions(), yMode};
      this.invalidate();
    },
  };

  readonly yRangeSetting: TrackSetting<YRange> = {
    descriptor: yRangeSettingDescriptor,
    getValue: () => this.getCounterOptions().yRange,
    setValue: (yRange) => {
      this.options = {...this.getCounterOptions(), yRange};
      this.invalidate();
    },
  };

  readonly yDisplaySetting: TrackSetting<YDisplay> = {
    descriptor: yDisplaySettingDescriptor,
    getValue: () => this.getCounterOptions().yDisplay,
    setValue: (yDisplay) => {
      this.options = {...this.getCounterOptions(), yDisplay};
      this.invalidate();
    },
  };

  readonly yRangeRoundingSetting: TrackSetting<YRangeRounding> = {
    descriptor: yRangeRoundingSettingDescriptor,
    getValue: () => this.getCounterOptions().yRangeRounding,
    setValue: (yRangeRounding) => {
      this.options = {...this.getCounterOptions(), yRangeRounding};
      this.invalidate();
    },
  };

  readonly chartHeightSizeSetting: TrackSetting<ChartHeightSize> = {
    descriptor: chartHeightSizeSettingDescriptor,
    getValue: () => this.getCounterOptions().chartHeightSize,
    setValue: (chartHeightSize) => {
      this.options = {...this.getCounterOptions(), chartHeightSize};
      this.invalidate();
    },
  };

  readonly settings: ReadonlyArray<TrackSetting<unknown>> = [
    this.yModeSetting,
    this.yRangeSetting,
    this.yDisplaySetting,
    this.yRangeRoundingSetting,
    this.chartHeightSizeSetting,
  ];

  async onUpdate({visibleWindow, size}: TrackRenderContext): Promise<void> {
    const windowSizePx = Math.max(1, size.width);
    const timespan = visibleWindow.toTimeSpan();
    const rawCountersKey = CacheKey.create(
      timespan.start,
      timespan.end,
      windowSizePx,
    );

    // If the visible time range is outside the cached area, requests
    // asynchronously new data from the SQL engine.
    await this.maybeRequestData(rawCountersKey);
  }

  render({ctx, size, timescale, colors}: TrackRenderContext): void {
    // In any case, draw whatever we have (which might be stale/incomplete).
    const limits = this.limits;
    const data = this.counters;

    if (data.timestamps.length === 0 || limits === undefined) {
      checkerboardExcept(
        ctx,
        this.getHeight(),
        0,
        size.width,
        timescale.timeToPx(this.countersKey.start),
        timescale.timeToPx(this.countersKey.end),
      );
      return;
    }

    assertTrue(data.timestamps.length === data.minDisplayValues.length);
    assertTrue(data.timestamps.length === data.maxDisplayValues.length);
    assertTrue(data.timestamps.length === data.lastDisplayValues.length);

    const timestamps = data.timestamps;
    const minValues = data.minDisplayValues;
    const maxValues = data.maxDisplayValues;
    const lastValues = data.lastDisplayValues;

    // Choose a range for the y-axis
    const {yRange, yMin, yMax, yLabel} = this.computeYRange(
      limits,
      data.displayValueRange,
    );

    const effectiveHeight = this.getHeight() - MARGIN_TOP;
    const endPx = size.width;

    // Use hue to differentiate the scale of the counter value
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const expCapped = Math.min(exp - 3, 9);
    const hue = (180 - Math.floor(expCapped * (180 / 6)) + 360) % 360;

    ctx.fillStyle = `hsla(${hue}, 45%, 50%, 0.6)`;
    ctx.strokeStyle = `hsl(${hue}, 45%, 50%)`;

    const calculateX = (ts: time) => {
      return Math.floor(timescale.timeToPx(ts));
    };
    const calculateY = (value: number) => {
      return (
        MARGIN_TOP +
        effectiveHeight -
        Math.round(((value - yMin) / yRange) * effectiveHeight)
      );
    };
    let zeroY;
    if (yMin >= 0) {
      zeroY = effectiveHeight + MARGIN_TOP;
    } else if (yMax < 0) {
      zeroY = MARGIN_TOP;
    } else {
      zeroY = effectiveHeight * (yMax / (yMax - yMin)) + MARGIN_TOP;
    }

    ctx.beginPath();
    const timestamp = Time.fromRaw(timestamps[0]);
    ctx.moveTo(Math.max(0, calculateX(timestamp)), zeroY);
    let lastDrawnY = zeroY;
    for (let i = 0; i < timestamps.length; i++) {
      const timestamp = Time.fromRaw(timestamps[i]);
      const x = Math.max(0, calculateX(timestamp));
      const minY = calculateY(minValues[i]);
      const maxY = calculateY(maxValues[i]);
      const lastY = calculateY(lastValues[i]);

      ctx.lineTo(x, lastDrawnY);
      if (minY === maxY) {
        assertTrue(lastY === minY);
        ctx.lineTo(x, lastY);
      } else {
        ctx.lineTo(x, minY);
        ctx.lineTo(x, maxY);
        ctx.lineTo(x, lastY);
      }
      lastDrawnY = lastY;
    }
    ctx.lineTo(endPx, lastDrawnY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    if (yMin < 0 && yMax > 0) {
      // Draw the Y=0 dashed line.
      ctx.strokeStyle = `hsl(${hue}, 10%, 71%)`;
      ctx.beginPath();
      ctx.setLineDash([2, 4]);
      ctx.moveTo(0, zeroY);
      ctx.lineTo(endPx, zeroY);
      ctx.closePath();
      ctx.stroke();
      ctx.setLineDash([]);
    }
    ctx.font = '10px Roboto Condensed';

    const hover = this.hover;
    if (hover !== undefined) {
      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      const rawXStart = calculateX(hover.ts);
      const xStart = Math.max(0, rawXStart);
      const xEnd =
        hover.tsEnd === undefined
          ? endPx
          : Math.floor(timescale.timeToPx(hover.tsEnd));
      const y =
        MARGIN_TOP +
        effectiveHeight -
        Math.round(
          ((hover.lastDisplayValue - yMin) / yRange) * effectiveHeight,
        );

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker if it would be visible.
      if (rawXStart >= -6) {
        ctx.beginPath();
        ctx.arc(
          xStart,
          y,
          3 /* r*/,
          0 /* start angle*/,
          2 * Math.PI /* end angle*/,
        );
        ctx.fill();
        ctx.stroke();
      }
    }

    // Write the Y scale on the top left corner.
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = colors.COLOR_BACKGROUND;
    ctx.globalAlpha = 0.6;
    ctx.fillRect(0, 0, 42, 18);
    ctx.globalAlpha = 1;
    ctx.fillStyle = colors.COLOR_TEXT;
    ctx.textAlign = 'left';
    ctx.fillText(`${yLabel}`, 4, 14);

    // TODO(hjd): Refactor this into checkerboardExcept
    {
      const counterEndPx = Infinity;
      // Grey out RHS.
      if (counterEndPx < endPx) {
        ctx.fillStyle = '#0000001f';
        ctx.fillRect(counterEndPx, 0, endPx - counterEndPx, this.getHeight());
      }
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(this.countersKey.start),
      timescale.timeToPx(this.countersKey.end),
    );
  }

  onMouseMove({x, timescale}: TrackMouseEvent) {
    const data = this.counters;
    if (data === undefined) return;
    const time = timescale.pxToHpTime(x);

    const [left, right] = searchSegment(data.timestamps, time.toTime());

    if (left === -1) {
      this.hover = undefined;
      return;
    }

    const ts = Time.fromRaw(data.timestamps[left]);
    const tsEnd =
      right === -1 ? undefined : Time.fromRaw(data.timestamps[right]);
    const lastDisplayValue = data.lastDisplayValues[left];
    this.hover = {
      ts,
      tsEnd,
      lastDisplayValue,
    };

    // Full redraw to update the tooltip
    raf.scheduleFullRedraw();
  }

  onMouseOut() {
    this.hover = undefined;
  }

  async onDestroy(): Promise<void> {
    await this.trash.asyncDispose();
  }

  // Compute the range of values to display and range label.
  private computeYRange(
    limits: CounterLimits,
    dataLimits: [number, number],
  ): {
    yMin: number;
    yMax: number;
    yRange: number;
    yLabel: string;
  } {
    const options = this.getCounterOptions();

    let yMin = limits.minDisplayValue;
    let yMax = limits.maxDisplayValue;

    if (options.yRange === 'viewport') {
      [yMin, yMax] = dataLimits;
    }

    if (options.yDisplay === 'zero') {
      yMin = Math.min(0, yMin);
      yMax = Math.max(0, yMax);
    }

    if (options.yOverrideMaximum !== undefined) {
      yMax = Math.max(options.yOverrideMaximum, yMax);
    }

    if (options.yOverrideMinimum !== undefined) {
      yMin = Math.min(options.yOverrideMinimum, yMin);
    }

    if (options.yRangeRounding === 'human_readable') {
      if (options.yDisplay === 'log') {
        yMax = Math.log(roundAway(Math.exp(yMax)));
        yMin = Math.log(roundAway(Math.exp(yMin)));
      } else {
        yMax = roundAway(yMax);
        yMin = roundAway(yMin);
      }
    }

    [yMin, yMax] = this.rangeSharer.share(options, [yMin, yMax]);

    let yLabel: string;

    if (options.yDisplay === 'minmax') {
      yLabel = 'min - max';
    } else {
      let max = yMax;
      let min = yMin;
      if (options.yDisplay === 'log') {
        max = Math.exp(max);
        min = Math.exp(min);
      }
      if (max < 0) {
        yLabel = toLabel(min - max);
      } else {
        yLabel = toLabel(max - min);
      }
    }

    const unit = this.unit;
    switch (options.yMode) {
      case 'value':
        yLabel += ` ${unit}`;
        break;
      case 'delta':
        yLabel += `\u0394${unit}`;
        break;
      case 'rate':
        yLabel += ` ${this.rateUnit}`;
        break;
      default:
        assertUnreachable(options.yMode);
    }

    if (options.yDisplay === 'log') {
      yLabel = `log(${yLabel})`;
    }

    return {
      yMin,
      yMax,
      yLabel,
      yRange: yMax - yMin,
    };
  }

  // The underlying table has `ts` and `value` columns.
  private getValueExpression(): string {
    const options = this.getCounterOptions();

    let valueExpr;
    switch (options.yMode) {
      case 'value':
        valueExpr = 'value';
        break;
      case 'delta':
        valueExpr = 'lead(value, 1, value) over (order by ts) - value';
        break;
      case 'rate':
        valueExpr =
          '(lead(value, 1, value) over (order by ts) - value) / ((lead(ts, 1, 100) over (order by ts) - ts) / 1e9)';
        break;
      default:
        assertUnreachable(options.yMode);
    }

    if (options.yDisplay === 'log') {
      return `ifnull(ln(${valueExpr}), 0)`;
    } else {
      return valueExpr;
    }
  }

  private getTableName(): string {
    return `counter_${this.trackUuid}`;
  }

  private async maybeRequestData(rawCountersKey: CacheKey) {
    if (rawCountersKey.isCoveredBy(this.countersKey)) {
      return; // We have the data already, no need to re-query.
    }

    const countersKey = rawCountersKey.normalize();
    if (!rawCountersKey.isCoveredBy(countersKey)) {
      throw new Error(
        `Normalization error ${countersKey.toString()} ${rawCountersKey.toString()}`,
      );
    }

    if (this.limits === undefined) {
      this.limits = await this.createTableAndFetchLimits(true);
    }

    const queryRes = await this.engine.query(`
      SELECT
        min_value as minDisplayValue,
        max_value as maxDisplayValue,
        last_ts as ts,
        last_value as lastDisplayValue
      FROM ${this.getTableName()}(
        ${countersKey.start},
        ${countersKey.end},
        ${countersKey.bucketSize}
      );
    `);

    const it = queryRes.iter({
      ts: LONG,
      minDisplayValue: NUM,
      maxDisplayValue: NUM,
      lastDisplayValue: NUM,
    });

    const numRows = queryRes.numRows();
    const data: CounterData = {
      timestamps: new BigInt64Array(numRows),
      minDisplayValues: new Float64Array(numRows),
      maxDisplayValues: new Float64Array(numRows),
      lastDisplayValues: new Float64Array(numRows),
      displayValueRange: [0, 0],
    };

    let min = 0;
    let max = 0;
    for (let row = 0; it.valid(); it.next(), row++) {
      data.timestamps[row] = Time.fromRaw(it.ts);
      data.minDisplayValues[row] = it.minDisplayValue;
      data.maxDisplayValues[row] = it.maxDisplayValue;
      data.lastDisplayValues[row] = it.lastDisplayValue;
      min = Math.min(min, it.minDisplayValue);
      max = Math.max(max, it.maxDisplayValue);
    }

    data.displayValueRange = [min, max];

    this.countersKey = countersKey;
    this.counters = data;

    raf.scheduleCanvasRedraw();
  }

  private async createTableAndFetchLimits(
    dropTable: boolean,
  ): Promise<CounterLimits> {
    const dropQuery = dropTable ? `drop table ${this.getTableName()};` : '';
    const displayValueQuery = await this.engine.query(`
      ${dropQuery}
      create virtual table ${this.getTableName()}
      using __intrinsic_counter_mipmap((
        select
          ts,
          ${this.getValueExpression()} as value
        from (${this.getSqlSource()})
      ));
      select
        min_value as minDisplayValue,
        max_value as maxDisplayValue
      from ${this.getTableName()}(
        trace_start(), trace_end() + 1, trace_dur() + 1
      );
    `);

    this.trash.defer(async () => {
      this.engine.tryQuery(`drop table if exists ${this.getTableName()}`);
    });

    const {minDisplayValue, maxDisplayValue} = displayValueQuery.firstRow({
      minDisplayValue: NUM,
      maxDisplayValue: NUM,
    });

    return {
      minDisplayValue,
      maxDisplayValue,
    };
  }

  get unit(): string {
    return this.getCounterOptions().unit ?? '';
  }

  get rateUnit(): string {
    return this.getCounterOptions().rateUnit ?? `\u0394${this.unit}/s`;
  }

  protected get engine() {
    return this.trace.engine;
  }
}
