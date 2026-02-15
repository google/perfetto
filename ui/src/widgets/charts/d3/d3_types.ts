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

import * as d3 from 'd3';

/**
 * Type aliases for common D3 selection types.
 * These reduce verbosity throughout the codebase.
 *
 * D3's selection types have 4 generic parameters:
 * - GElement: The type of element being selected
 * - Datum: The type of data bound to the element
 * - PElement: The type of parent element
 * - PDatum: The type of data bound to the parent
 *
 * For chart rendering without data binding, we use (unknown, null, undefined).
 */

export type D3SVGSelection = d3.Selection<
  SVGSVGElement,
  unknown,
  null,
  undefined
>;

export type D3GroupSelection = d3.Selection<
  SVGGElement,
  unknown,
  null,
  undefined
>;

/**
 * Select an SVG element and return a typed D3 selection.
 *
 * Cast is required: render() receives generic SVGElement but we need
 * SVGSVGElement for SelectionClipPaths. Runtime type is always SVGSVGElement.
 */
export function selectSVG(element: SVGElement): D3SVGSelection {
  return d3.select(element as SVGSVGElement);
}

/**
 * Select an SVG group element and return a typed D3 selection.
 */
export function selectGroup(element: SVGGElement): D3GroupSelection {
  return d3.select(element);
}

/**
 * Clear a D3 brush selection.
 *
 * Cast is required: g.select('.brush') returns Selection<Element, ...>
 * but brush.clear() expects Selection<SVGGElement, ...>.
 */
export function clearBrush(
  brush: d3.BrushBehavior<unknown>,
  g: D3GroupSelection,
): void {
  brush.clear(g.select('.brush') as unknown as D3GroupSelection);
}
