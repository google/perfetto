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
import {productEmbedder} from './product_stub';

/**
 * Returns the product's Embedder when built with `ui/build --product`.
 * Otherwise uses PerfettoUiEmbedder when running on ui.perfetto.dev or
 * localhost, and DefaultEmbedder elsewhere.
 */
export function createEmbedder(): Embedder {
  if (productEmbedder !== undefined) {
    return productEmbedder();
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
