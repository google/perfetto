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

import type {Embedder} from './embedder';
import {DefaultEmbedder} from './default_embedder';
import {PerfettoUiEmbedder} from './perfetto_ui_embedder';
import {PRODUCTS} from '../../products';

/**
 * Returns the appropriate Embedder based on the current origin.
 * Uses PerfettoUiEmbedder when running on ui.perfetto.dev or localhost,
 * and DefaultEmbedder otherwise.
 */
export function createEmbedder(): Embedder {
  const product = PRODUCTS.get(getProductId() ?? '');
  if (product !== undefined) {
    return product();
  }
  const origin = self.location?.origin ?? '';
  if (
    origin.endsWith('.perfetto.dev') ||
    origin.startsWith('http://localhost:') ||
    origin.startsWith('http://127.0.0.1:')
  ) {
    return new PerfettoUiEmbedder();
  }
  return new DefaultEmbedder();
}

// Picks the product, in order: the <meta name="perfetto-product"> tag written
// by `ui/build --product`, the `?product=` query arg (for development), or
// the `<product>.perfetto.dev` hostname.
function getProductId(): string | undefined {
  if (typeof document !== 'undefined') {
    const meta = document.querySelector('meta[name="perfetto-product"]');
    const content = meta?.getAttribute('content');
    if (content) return content;
  }
  const loc = self.location;
  if (loc === undefined) return undefined;
  const fromQuery = new URLSearchParams(loc.search).get('product');
  if (fromQuery) return fromQuery;
  const m = /^([a-z0-9-]+)\.perfetto\.dev$/.exec(loc.hostname);
  return m?.[1];
}
