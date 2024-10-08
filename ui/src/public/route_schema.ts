// Copyright (C) 2024 The Android Open Source Project
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

import {z} from 'zod';

// We use .catch(undefined) on every field below to make sure that passing an
// invalid value doesn't invalidate the other keys which might be valid.
// Zod default behaviour is atomic: either everything validates correctly or
// the whole parsing fails.
export const ROUTE_SCHEMA = z
  .object({
    // The local_cache_key is special and is persisted across navigations.
    local_cache_key: z.string().optional().catch(undefined),

    // These are transient and are really set only on startup.

    // Are we loading a trace via ABT.
    openFromAndroidBugTool: z.boolean().optional().catch(undefined),

    // For permalink hash.
    s: z.string().optional().catch(undefined),

    // DEPRECATED: for #!/record?p=cpu subpages (b/191255021).
    p: z.string().optional().catch(undefined),

    // For fetching traces from Cloud Storage or local servers
    // as with record_android_trace.
    url: z.string().optional().catch(undefined),

    // For connecting to a trace_processor_shell --httpd instance running on a
    // non-standard port. This requires the CSP_WS_PERMISSIVE_PORT flag to relax
    // the Content Security Policy.
    rpc_port: z.string().regex(/\d+/).optional().catch(undefined),

    // Override the referrer. Useful for scripts such as
    // record_android_trace to record where the trace is coming from.
    referrer: z.string().optional().catch(undefined),

    // For the 'mode' of the UI. For example when the mode is 'embedded'
    // some features are disabled.
    mode: z.enum(['embedded']).optional().catch(undefined),

    // Should we hide the sidebar?
    hideSidebar: z.boolean().optional().catch(undefined),

    // A comma-separated list of plugins to enable for the current session.
    enablePlugins: z.string().optional().catch(undefined),

    // Deep link support
    ts: z.string().optional().catch(undefined),
    dur: z.string().optional().catch(undefined),
    tid: z.string().optional().catch(undefined),
    pid: z.string().optional().catch(undefined),
    query: z.string().optional().catch(undefined),
    visStart: z.string().optional().catch(undefined),
    visEnd: z.string().optional().catch(undefined),
  })
  // default({}) ensures at compile-time that every entry is either optional or
  // has a default value.
  .default({});

export type RouteArgs = z.infer<typeof ROUTE_SCHEMA>;
