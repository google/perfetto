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
import {ALL_FILTER_OPS, FilterAttrs, FilterOperation} from './filter';
import {
  Aggregation,
  GroupByAggregationAttrsToProto,
  AggregationsOperatorAttrs,
  AggregationsOperator,
} from './aggregations';
import {FilterDefinition} from '../../../../components/widgets/data_grid/common';
import {Button, ButtonVariant} from '../../../../widgets/button';
import protos from '../../../../protos';
import {ColumnInfo} from '../column_info';

export interface OperatorAttrs {
  filter: FilterAttrs;
  groupby: AggregationsOperatorAttrs;
}

export class Operator implements m.ClassComponent<OperatorAttrs> {
  private showAggregations = false;

  view({attrs}: m.CVnode<OperatorAttrs>): m.Children {
    return m('.pf-exp-query-operations', [
      m(FilterOperation, attrs.filter),
      this.showAggregations
        ? m(AggregationsOperator, attrs.groupby)
        : m(Button, {
            label: 'Aggregate data',
            onclick: () => {
              this.showAggregations = true;
            },
            variant: ButtonVariant.Filled,
          }),
    ]);
  }
}

export function createFiltersProto(
  filters: FilterDefinition[],
  sourceCols: ColumnInfo[],
): protos.PerfettoSqlStructuredQuery.Filter[] | undefined {
  if (filters.length === 0) {
    return undefined;
  }

  const protoFilters: protos.PerfettoSqlStructuredQuery.Filter[] = filters.map(
    (f: FilterDefinition): protos.PerfettoSqlStructuredQuery.Filter => {
      const result = new protos.PerfettoSqlStructuredQuery.Filter();
      result.columnName = f.column;

      const op = ALL_FILTER_OPS.find((o) => o.displayName === f.op);
      if (op === undefined) {
        // Should be handled by validation before this.
        throw new Error(`Unknown filter operator: ${f.op}`);
      }
      result.op = op.proto;

      if ('value' in f) {
        const value = f.value;
        const col = sourceCols.find((c) => c.name === f.column);
        if (typeof value === 'string') {
          result.stringRhs = [value];
        } else if (typeof value === 'number' || typeof value === 'bigint') {
          if (col && (col.type === 'long' || col.type === 'int')) {
            result.int64Rhs = [Number(value)];
          } else {
            result.doubleRhs = [Number(value)];
          }
        }
        // Not handling Uint8Array here. The original FilterToProto also didn't seem to.
      }
      return result;
    },
  );
  return protoFilters;
}

export function createGroupByProto(
  groupByColumns: ColumnInfo[],
  aggregations: Aggregation[],
): protos.PerfettoSqlStructuredQuery.GroupBy | undefined {
  if (!groupByColumns.find((c) => c.checked)) return;

  const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
  groupByProto.columnNames = groupByColumns
    .filter((c) => c.checked)
    .map((c) => c.column.name);

  for (const agg of aggregations) {
    agg.isValid = validateAggregation(agg);
  }
  groupByProto.aggregates = aggregations
    .filter((agg) => agg.isValid)
    .map(GroupByAggregationAttrsToProto);
  return groupByProto;
}

// Both 'column' and 'aggregationOp' must be present for an aggregation to be considered valid.
// This ensures that the aggregation operation is applied to a specific column.
function validateAggregation(aggregation: Aggregation): boolean {
  if (!aggregation.column || !aggregation.aggregationOp) return false;
  return true;
}
