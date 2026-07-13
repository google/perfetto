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

import z from 'zod';

export const NodeSchema = z.object({
  name: z.string().optional(),
  parameterized: z.boolean().optional(),
  get schema() {
    return z.record(z.string(), NodeSchema).optional();
  },
});

export const SqlSchema = z.object({
  join: z.function().optional(),
  select: z.function().optional(),
  get schema() {
    return z.object({sql: z.string(), schema: SqlSchema}).optional();
  },
});

export const DataGridConfigSchema = z.object({
  schema: z.record(z.string(), NodeSchema),
  cols: z.array(
    z.object({
      field: z.array(z.string()),
      id: z.string(),
      colId: z.string(),
    }),
  ),
  sql: z.object({
    sql: z.string(),
    schema: z.record(z.string(), SqlSchema).optional(),
  }),
  pivot: z
    .object({
      groupby: z.array(z.string()),
      aggregate: z.array(
        z.object({
          colId: z.string(),
          func: z.enum(['sum', 'count', 'avg', 'min', 'max']),
        }),
      ),
    })
    .optional(),
});
