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

import {SortDirection} from '../common/state';

interface OrderClause {
  fieldName: string;
  direction?: SortDirection;
}

// Interface for defining constraints which can be passed to a SQL query.
export interface SQLConstraints {
      where?: string[];
  orderBy?: OrderClause[];
  limit?: number;
}

// Formatting given constraints into a string which can be injected into
// SQL query.
export function constraintsToQueryFragment(c: SQLConstraints): string {
  const result: string[] = [];
  if (c.where && c.where.length > 0) {
    result.push(`WHERE ${c.where.join(' and ')}`);
  }
  if (c.orderBy && c.orderBy.length > 0) {
    const orderBys = c.orderBy.map((clause) => {
      const direction = clause.direction ? ` ${clause.direction}` : '';
      return `${clause.fieldName}${direction}`;
    });
    result.push(`ORDER BY ${orderBys.join(', ')}`);
  }
  if (c.limit) {
    result.push(`LIMIT ${c.limit}`);
  }
  return result.join('\n');
}

// Trace Processor returns number | null for NUM_NULL, while most of the UI
// code uses number | undefined. This functions provides a short-hand
// conversion.
// TODO(altimin): Support NUM_UNDEFINED as a first-class citizen.
export function fromNumNull(n: number|null): number|undefined {
  if (n === null) {
    return undefined;
  }
  return n;
}
