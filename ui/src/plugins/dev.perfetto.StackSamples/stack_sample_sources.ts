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

export interface StackSampleSourceSchema {
  readonly source: string;
  readonly title: string;
  // Also defines the automatic-selection preference (lowest wins).
  readonly order: number;
}

export const STACK_SAMPLE_SOURCE_SCHEMAS: ReadonlyArray<StackSampleSourceSchema> =
  [
    {source: 'linux.perf', title: 'Perf', order: 0},
    {source: 'instruments', title: 'Instruments', order: 10},
    {source: 'chrome', title: 'Chrome CPU Profile', order: 20},
    {source: 'legacy_v8', title: 'Legacy V8 CPU Profile', order: 20},
    {source: 'gecko', title: 'Gecko CPU Profile', order: 20},
    {source: 'simpleperf', title: 'Simpleperf CPU Profile', order: 20},
    {source: 'perf_text', title: 'Perf Text CPU Profile', order: 20},
  ];

const SCHEMA_BY_SOURCE = new Map(
  STACK_SAMPLE_SOURCE_SCHEMAS.map((schema) => [schema.source, schema]),
);

export function getStackSampleSourceSchema(
  source: string,
): StackSampleSourceSchema {
  return (
    SCHEMA_BY_SOURCE.get(source) ?? {
      source,
      title: humanizeSource(source),
      order: 100,
    }
  );
}

function humanizeSource(source: string): string {
  const name = source
    .split(/[._-]+/)
    .filter((part) => part.length > 0)
    .map((part) => part[0].toUpperCase() + part.slice(1))
    .join(' ');
  return name || 'Unknown';
}
