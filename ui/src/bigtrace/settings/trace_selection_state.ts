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

import {linkNameFirst} from './column_order';

// Everything about the trace selection is per-query — it lives on the tab and
// its saved presets. The tab's traceMetadataColumns double as the grid's
// shown columns: what the grid shows is what a run attaches to result rows.

// Subset of a /trace_metadata_schema column the resolver below needs.
interface SchemaColumn {
  readonly name: string;
  readonly defaultVisible: boolean;
}

// Resolves a tab's chosen trace columns against the live schema
// (link first): null → defaultVisible (so an untouched picker shows defaults);
// [...] → these ∩ schema; [] → nothing.
export function effectiveQueryColumns(
  chosen: readonly string[] | null,
  schema: ReadonlyArray<SchemaColumn>,
): string[] {
  if (chosen === null) {
    return linkNameFirst(
      schema.filter((c) => c.defaultVisible).map((c) => c.name),
    );
  }
  const known = new Set(schema.map((c) => c.name));
  return linkNameFirst(chosen.filter((c) => known.has(c)));
}
