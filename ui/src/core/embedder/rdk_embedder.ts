// Copyright 2026 Comcast Cable Communications Management, LLC
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

import type {Embedder, BrandingBadge} from './embedder';
import {rdkPlugins} from './rdk_plugins';

/**
 * Branding configuration for RDK
 */
class RDKBranding implements BrandingBadge {
  readonly text = 'RDK';

  /** CSS color for the text, e.g. "#e07020". */
  readonly color = '#00b3dc';

  /** The RDK 4 bars logo SVG as a data URI. */
  readonly image =
    'data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiPz4KPHN2ZyBpZD0iTGF5ZXJfMSIgZGF0YS1uYW1lPSJMYXllciAxIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB2aWV3Qm94PSIwIDAgMjM1IDIwNiI+CiAgPGcgY2xhc3M9ImNscy04Ij4KICAgIDxnIGNsYXNzPSJjbHMtNCI+CiAgICAgIDxnPgogICAgICAgIDxwYXRoIGQ9Ik0yMzQuMiwwSDB2MzkuMDdoMjM0LjJWMFoiIGZpbGw9IiMwMGIwZGEiLz4KICAgICAgICA8cGF0aCBkPSJNMjM0LjIsMTY3LjAySDB2MzkuMDdoMjM0LjJ2LTM5LjA3WiIgZmlsbD0iI2YzN2QzMSIvPgogICAgICAgIDxwYXRoIGQ9Ik0yMzQuMiwxMTEuMzNIMHYzOS4wN2gyMzQuMnYtMzkuMDdaIiBmaWxsPSIjOTRjNzNkIi8+CiAgICAgICAgPHBhdGggZD0iTTIzNC4yLDU1LjY5SDB2MzkuMDdoMjM0LjJ2LTM5LjA3WiIgZmlsbD0iI2ZjYmIzMSIvPgogICAgICA8L2c+CiAgICA8L2c+CiAgPC9nPgo8L3N2Zz4=';
}

/**
 * RDK embedder implementation, the only change is for the MessageChannel
 * support.
 */
export class RDKEmbedder implements Embedder {
  readonly analyticsId = undefined;
  readonly extensionServer = undefined;
  readonly brandingBadge = new RDKBranding();
  readonly defaultPlugins = rdkPlugins;
}
