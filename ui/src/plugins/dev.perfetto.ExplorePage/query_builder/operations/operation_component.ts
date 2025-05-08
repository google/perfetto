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
} from './groupy_by';
import protos from '../../../../protos';
import {ColumnControllerRow} from '../column_controller';
import {Section} from '../../../../widgets/section';

export interface OperatorAttrs {
  filter: FilterAttrs;
  groupby: GroupByAttrs;
}

export class Operator implements m.ClassComponent<OperatorAttrs> {
  view({attrs}: m.CVnode<OperatorAttrs>): m.Children {
    return m(
      '.explore-page__rowish',
      m(Section, {title: 'Filters'}, m(FilterOperation, attrs.filter)),
      m(Section, {title: 'Aggregation'}, m(GroupByOperation, attrs.groupby)),
    );
  }
}

export function createFiltersProto(
  filters: Filter[],
): protos.PerfettoSqlStructuredQuery.Filter[] | undefined {
  const protos = filters
    .filter((f) => validateFilter(f))
    .map((f) => FilterToProto(f));
  return protos.length !== 0 ? protos : undefined;
}

export function createGroupByProto(
  groupByColumns: ColumnControllerRow[],
  aggregations: GroupByAgg[],
): protos.PerfettoSqlStructuredQuery.GroupBy | undefined {
  if (!groupByColumns.find((c) => c.checked)) return;

  const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
  groupByProto.columnNames = groupByColumns
    .filter((c) => c.checked)
    .map((c) => c.column.name);

  groupByProto.aggregates = aggregations
    .filter((agg) => validateAggregation(agg))
    .map(GroupByAggregationAttrsToProto);
  return groupByProto;
}

function validateAggregation(aggregation: GroupByAgg): boolean {
  if (!aggregation.column) return false;
  return true;
}

function validateFilter(filter: Filter): boolean {
  if (!filter.columnName.checked) return false;
  if (
    filter.stringsRhs.length === 0 &&
    filter.doubleRhs.length === 0 &&
    filter.intRhs.length === 0
  ) {
    return false;
  }
  return true;
}
