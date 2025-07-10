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
import {Filter, FilterAttrs, FilterOperation, FilterToProto} from './filter';
import {
  GroupByAgg,
  GroupByAggregationAttrsToProto,
  GroupByAttrs,
  GroupByOperation,
} from './group_by';
import protos from '../../../../protos';
import {ColumnInfo} from '../column_info';

export interface OperatorAttrs {
  filter: FilterAttrs;
  groupby: GroupByAttrs;
}

export class Operator implements m.ClassComponent<OperatorAttrs> {
  view({attrs}: m.CVnode<OperatorAttrs>): m.Children {
    return m(
      'div',
      {
        class: 'pf-query-operations',
      },
      m(
        'div',
        {class: 'section'},
        m('h2', 'Aggregations'),
        m(GroupByOperation, attrs.groupby),
      ),
      m(
        'div',
        {class: 'section'},
        m('h2', 'Filters'),
        m(FilterOperation, attrs.filter),
      ),
    );
  }
}

export function createFiltersProto(
  filters: Filter[],
): protos.PerfettoSqlStructuredQuery.Filter[] | undefined {
  for (const filter of filters) {
    filter.isValid = validateFilter(filter);
  }
  const protos = filters.filter((f) => f.isValid).map((f) => FilterToProto(f));
  return protos.length !== 0 ? protos : undefined;
}

export function createGroupByProto(
  groupByColumns: ColumnInfo[],
  aggregations: GroupByAgg[],
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

function validateAggregation(aggregation: GroupByAgg): boolean {
  if (!aggregation.column || !aggregation.aggregationOp) return false;
  return true;
}

function validateFilter(filter: Filter): boolean {
  if (!filter.columnName || !filter.filterOp) return false;
  if (
    filter.stringsRhs.length === 0 &&
    filter.doubleRhs.length === 0 &&
    filter.intRhs.length === 0
  ) {
    return false;
  }
  return true;
}
