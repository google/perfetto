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

import * as d3 from 'd3';
import {DataSource} from './source';
import {
  Filter,
  Row,
  ChartSpec,
  ChartType,
  FilterOp,
  AggregationFunction,
} from './types';

export class MemorySource implements DataSource {
  constructor(private data: Row[]) {}

  async query(filters: Filter[], spec: ChartSpec): Promise<Row[]> {
    let result = this.applyFilters(this.data, filters);
    result = this.aggregateBySpec(result, spec);
    return result;
  }

  private applyFilters(data: Row[], filters: Filter[]): Row[] {
    return data.filter((row) => {
      return filters.every((f) => {
        const value = row[f.col];
        switch (f.op) {
          case FilterOp.Eq:
            return value === f.val;
          case FilterOp.NotEq:
            return value !== f.val;
          case FilterOp.Lt:
            return f.val !== null && value != null && value < f.val;
          case FilterOp.Lte:
            return f.val !== null && value != null && value <= f.val;
          case FilterOp.Gt:
            return f.val !== null && value != null && value > f.val;
          case FilterOp.Gte:
            return f.val !== null && value != null && value >= f.val;
          case FilterOp.In:
            if (f.val === null || !Array.isArray(f.val)) return false;
            return (f.val as (string | number)[]).includes(
              value as string | number,
            );
          case FilterOp.NotIn:
            if (f.val === null || !Array.isArray(f.val)) return false;
            return !(f.val as (string | number)[]).includes(
              value as string | number,
            );
          case FilterOp.Glob: {
            if (typeof f.val !== 'string') return false;
            // Escape special regex characters then convert globs (*) to regex wildcards (.*)
            // e.g. "file.txt" -> "file\.txt" -> "^file\.txt$"
            // e.g. "*.txt" -> ".*\.txt" -> "^.*\.txt$"
            const escaped = f.val.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
            const pattern = `^${escaped.replace(/\\\*/g, '.*')}$`;
            return new RegExp(pattern).test(String(value));
          }
          default:
            return true;
        }
      });
    });
  }

  private aggregateBySpec(data: Row[], spec: ChartSpec): Row[] {
    switch (spec.type) {
      case ChartType.Bar:
        return this.aggregateBar(data, spec);
      case ChartType.Donut:
        return this.aggregateDonut(data, spec);
      case ChartType.Heatmap:
        return this.aggregateHeatmap(data, spec);
      case ChartType.Histogram:
        return this.aggregateHistogram(data, spec);
      case ChartType.Cdf:
        return this.aggregateCdf(data, spec);
      case ChartType.Boxplot:
        return this.aggregateBoxplot(data, spec);
      case ChartType.Violin:
        return this.aggregateViolin(data, spec);
      case ChartType.Line:
        return this.aggregateLine(data, spec);
      default:
        // Scatter returns data as-is (point-level)
        return data;
    }
  }

  private aggregateBar(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Bar}>,
  ): Row[] {
    const groupBy = [spec.x];
    if (spec.groupBy) {
      groupBy.push(spec.groupBy);
    }
    return this.aggregate(data, spec.aggregation, spec.y, groupBy);
  }

  private aggregateDonut(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Donut}>,
  ): Row[] {
    return this.aggregate(data, spec.aggregation, spec.value, [spec.category]);
  }

  private aggregateHeatmap(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Heatmap}>,
  ): Row[] {
    return this.aggregate(data, spec.aggregation, spec.value, [spec.x, spec.y]);
  }

  private aggregate(
    data: Row[],
    fn: AggregationFunction,
    field: string,
    groupBy: string[],
  ): Row[] {
    if (groupBy.length === 0) {
      const value = this.computeAggregation(data, fn, field);
      return [{[field]: value}];
    }

    const grouped = d3.group(data, ...groupBy.map((col) => (d: Row) => d[col]));
    const result: Row[] = [];

    const processGroup = (
      group: Row[] | Map<unknown, unknown>,
      keys: (string | number | boolean)[],
      depth: number,
    ) => {
      if (depth === groupBy.length) {
        const value = this.computeAggregation(group as Row[], fn, field);
        const row: Row = {};
        groupBy.forEach((col, i) => {
          row[col] = keys[i];
        });
        row[field] = value;
        result.push(row);
      } else {
        for (const [key, subgroup] of group as Map<unknown, unknown>) {
          processGroup(
            subgroup as Row[] | Map<unknown, unknown>,
            [...keys, key as string | number | boolean],
            depth + 1,
          );
        }
      }
    };

    processGroup(grouped, [], 0);
    return result;
  }

  private computeAggregation(
    data: Row[],
    fn: AggregationFunction,
    field: string,
  ): number {
    const values = data.map((d) => Number(d[field])).filter((v) => !isNaN(v));

    switch (fn) {
      case AggregationFunction.Sum:
        return d3.sum(values);
      case AggregationFunction.Avg:
        return d3.mean(values) ?? 0;
      case AggregationFunction.Count:
        return data.length;
      case AggregationFunction.Min:
        return d3.min(values) ?? 0;
      case AggregationFunction.Max:
        return d3.max(values) ?? 0;
      default:
        return 0;
    }
  }

  private aggregateHistogram(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Histogram}>,
  ): Row[] {
    const values = data
      .map((d) => Number(d[spec.x]))
      .filter((v) => !isNaN(v) && isFinite(v));

    if (values.length === 0) return [];

    const numBins = spec.bins ?? 20;
    const min = d3.min(values)!;
    const max = d3.max(values)!;
    const binWidth = (max - min) / numBins;

    // Create bins manually to match SQL output format
    const bins: Row[] = [];
    for (let i = 0; i < numBins; i++) {
      const x0 = min + i * binWidth;
      const x1 = min + (i + 1) * binWidth;
      const count = values.filter(
        (v) => v >= x0 && (i === numBins - 1 ? v <= x1 : v < x1),
      ).length;
      bins.push({x0, x1, count});
    }

    return bins;
  }

  private aggregateCdf(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Cdf}>,
  ): Row[] {
    if (spec.colorBy) {
      // Group by color field and compute CDF per group
      const grouped = d3.group(data, (d) => d[spec.colorBy!]);
      const result: Row[] = [];

      for (const [group, groupData] of grouped) {
        const values = groupData
          .map((d) => Number(d[spec.x]))
          .filter((v) => !isNaN(v) && isFinite(v))
          .sort(d3.ascending);

        const n = values.length;
        values.forEach((value, i) => {
          result.push({
            value,
            probability: (i + 1) / n,
            group: group as string | number,
          });
        });
      }

      return result;
    } else {
      // Single CDF
      const values = data
        .map((d) => Number(d[spec.x]))
        .filter((v) => !isNaN(v) && isFinite(v))
        .sort(d3.ascending);

      const n = values.length;
      return values.map((value, i) => ({
        value,
        probability: (i + 1) / n,
      }));
    }
  }

  private aggregateBoxplot(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Boxplot}>,
  ): Row[] {
    const grouped = d3.group(data, (d) => d[spec.x]);
    const result: Row[] = [];

    for (const [group, groupData] of grouped) {
      const values = groupData
        .map((d) => Number(d[spec.y]))
        .filter((v) => !isNaN(v) && isFinite(v))
        .sort(d3.ascending);

      if (values.length === 0) continue;

      const min = d3.min(values)!;
      const max = d3.max(values)!;
      const q1 = d3.quantile(values, 0.25)!;
      const median = d3.quantile(values, 0.5)!;
      const q3 = d3.quantile(values, 0.75)!;

      result.push({
        group: group as string | number,
        min,
        q1,
        median,
        q3,
        max,
      });
    }

    return result;
  }

  private aggregateViolin(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Violin}>,
  ): Row[] {
    const grouped = d3.group(data, (d) => d[spec.x]);
    const result: Row[] = [];

    for (const [group, groupData] of grouped) {
      const values = groupData
        .map((d) => Number(d[spec.y]))
        .filter((v) => !isNaN(v) && isFinite(v))
        .sort(d3.ascending);

      if (values.length === 0) continue;

      const min = d3.min(values)!;
      const max = d3.max(values)!;
      const q1 = d3.quantile(values, 0.25)!;
      const median = d3.quantile(values, 0.5)!;
      const q3 = d3.quantile(values, 0.75)!;
      const p90 = d3.quantile(values, 0.9)!;
      const p95 = d3.quantile(values, 0.95)!;
      const p99 = d3.quantile(values, 0.99)!;

      // Compute KDE
      const density = this.computeKDE(values, min, max);

      result.push({
        group: group as string | number,
        min,
        max,
        q1,
        median,
        q3,
        p90,
        p95,
        p99,
        density: JSON.stringify(density), // Store as JSON string for Row compatibility
      });
    }

    return result;
  }

  private aggregateLine(
    data: Row[],
    spec: Extract<ChartSpec, {type: ChartType.Line}>,
  ): Row[] {
    // If no aggregation is specified, return data as-is (point-level)
    if (spec.aggregation === undefined) {
      return data;
    }

    // Aggregate by x and optionally colorBy
    const groupBy = [spec.x];
    if (spec.colorBy) {
      groupBy.push(spec.colorBy);
    }
    return this.aggregate(data, spec.aggregation, spec.y, groupBy);
  }

  private computeKDE(
    values: number[],
    min: number,
    max: number,
  ): [number, number][] {
    const bandwidth = 0.5; // DEFAULT_KDE_BANDWIDTH
    const samplePoints = 50; // DEFAULT_KDE_SAMPLE_POINTS
    const kernel = this.epanechnikovKernel(bandwidth);
    const ticks = d3.ticks(min, max, samplePoints);

    return ticks.map((x) => [x, d3.mean(values, (v) => kernel(x - v)) ?? 0]);
  }

  private epanechnikovKernel(bandwidth: number): (v: number) => number {
    return (v: number) => {
      const u = v / bandwidth;
      const kernelConst = 0.75; // EPANECHNIKOV_KERNEL_CONST
      return Math.abs(u) <= 1 ? (kernelConst * (1 - u * u)) / bandwidth : 0;
    };
  }
}
