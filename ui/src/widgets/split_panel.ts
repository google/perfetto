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
import {classNames} from '../base/classnames';

type SplitSizePixels = {
  readonly pixels: number;
};

type SplitSizePercent = {
  readonly percent: number;
};

// Split configuration - either percentage or fixed size in pixels.
type SplitSize = SplitSizePixels | SplitSizePercent;

export interface SplitPanelAttrs {
  // Layout direction. Default is 'horizontal'.
  readonly direction?: 'horizontal' | 'vertical';

  // Controls which panel has the controlled size. Default is 'first'.
  readonly controlledPanel?: 'first' | 'second';

  // Initial split configuration for uncontrolled mode. Only read once in
  // oninit. Ignored if `split` is provided.
  readonly initialSplit?: SplitSize;

  // Controlled split configuration. When provided, the component will use this
  // value directly and the parent must update it via onResize to see changes.
  readonly split?: SplitSize;

  // Minimum size in pixels for each panel
  readonly minSize?: number;

  // Additional CSS class for the root element.
  readonly className?: string;

  // Content for the first panel
  readonly firstPanel: m.Children;

  // Content for the second panel
  readonly secondPanel: m.Children;

  // Callback invoked when the user resizes a panel in both controlled and
  // uncontrolled modes.
  readonly onResize?: (size: number) => void;
}

// Factory function to create SplitPanel instances with their own state
export function SplitPanel(
  vnode: m.Vnode<SplitPanelAttrs>,
): m.Component<SplitPanelAttrs> {
  // Internal state for uncontrolled mode
  let internalPercent = 50;
  let internalFixedPx = 150;
  let isResizing = false;

  const initial = vnode.attrs.initialSplit ?? {percent: 50};
  if ('pixels' in initial) {
    internalFixedPx = initial.pixels;
  } else if ('percent' in initial) {
    internalPercent = initial.percent;
  }

  return {
    view(vnode) {
      const {
        direction = 'horizontal',
        minSize = 50,
        split,
        initialSplit,
        firstPanel,
        secondPanel,
        controlledPanel: panel,
      } = vnode.attrs;

      // Determine if we're in controlled or uncontrolled mode
      const isControlled = split !== undefined;
      const effectiveSplit = split ?? initialSplit ?? {percent: 50};
      const isPixelMode = 'pixels' in effectiveSplit;
      const controlledPanel = panel ?? 'first';

      const containerClasses = classNames(
        'pf-split-panel',
        `pf-split-${direction}`,
        vnode.attrs.className,
      );
      const handleSize = 4;

      // Get current size - from controlled prop or internal state
      let currentPercent: number;
      let currentPixels: number;
      if (isControlled && isPixelMode) {
        currentPixels = effectiveSplit.pixels;
        currentPercent = 50; // unused in pixel mode
      } else if (isControlled && 'percent' in effectiveSplit) {
        currentPercent = effectiveSplit.percent;
        currentPixels = 150; // unused in percent mode
      } else {
        currentPercent = internalPercent;
        currentPixels = internalFixedPx;
      }

      let firstStyle: Record<string, string>;
      let secondStyle: Record<string, string>;

      // Use CSS min/max width/height to enforce size constraints
      const minProp = direction === 'horizontal' ? 'minWidth' : 'minHeight';
      const maxProp = direction === 'horizontal' ? 'maxWidth' : 'maxHeight';
      const maxSize = `calc(100% - ${minSize}px - ${handleSize}px)`;

      if (isPixelMode) {
        // Pixel mode - one panel fixed, one flexible
        if (controlledPanel === 'first') {
          firstStyle = {
            flex: `0 0 ${currentPixels}px`,
            [minProp]: `${minSize}px`,
            [maxProp]: maxSize,
          };
          secondStyle = {flex: '1 1 0', [minProp]: `${minSize}px`};
        } else {
          firstStyle = {flex: '1 1 0', [minProp]: `${minSize}px`};
          secondStyle = {
            flex: `0 0 ${currentPixels}px`,
            [minProp]: `${minSize}px`,
            [maxProp]: maxSize,
          };
        }
      } else {
        // Percentage mode
        const firstPercent =
          controlledPanel === 'first' ? currentPercent : 100 - currentPercent;
        firstStyle = {
          flex: `0 0 calc(${firstPercent}% - ${handleSize / 2}px)`,
          [minProp]: `${minSize}px`,
          [maxProp]: maxSize,
        };
        secondStyle = {
          flex: `0 0 calc(${100 - firstPercent}% - ${handleSize / 2}px)`,
          [minProp]: `${minSize}px`,
          [maxProp]: maxSize,
        };
      }

      const onPointerDown = (e: PointerEvent) => {
        e.preventDefault();
        const handle = e.currentTarget as HTMLElement;
        handle.setPointerCapture(e.pointerId);
        isResizing = true;
        document.body.style.cursor =
          direction === 'horizontal' ? 'col-resize' : 'row-resize';
        document.body.style.userSelect = 'none';
        handle.classList.add('pf-split-handle--active');
      };

      const onPointerMove = (e: PointerEvent) => {
        if (!isResizing) return;

        const handle = e.currentTarget as HTMLElement;
        const container = handle.parentElement!;
        const rect = container.getBoundingClientRect();
        const containerSize =
          direction === 'horizontal' ? rect.width : rect.height;

        // Calculate position from start of container
        let pos: number;
        if (direction === 'horizontal') {
          pos = e.clientX - rect.left;
        } else {
          pos = e.clientY - rect.top;
        }

        if (isPixelMode) {
          // Pixel mode
          let newSize: number;
          if (controlledPanel === 'first') {
            newSize = pos;
          } else {
            newSize = containerSize - pos;
          }

          // Clamp to min/max
          newSize = Math.max(
            minSize,
            Math.min(containerSize - minSize - handleSize, newSize),
          );

          // Update internal state only in uncontrolled mode
          if (!isControlled) {
            internalFixedPx = newSize;
          }

          if (vnode.attrs.onResize) {
            vnode.attrs.onResize(newSize);
          }
        } else {
          // Percentage mode
          let newPercent = (pos / containerSize) * 100;

          // If controlling second panel, invert the percentage
          if (controlledPanel === 'second') {
            newPercent = 100 - newPercent;
          }

          const minPercent = (minSize / containerSize) * 100;
          const maxPercent = 100 - minPercent;
          newPercent = Math.max(minPercent, Math.min(maxPercent, newPercent));

          // Update internal state only in uncontrolled mode
          if (!isControlled) {
            internalPercent = newPercent;
          }

          if (vnode.attrs.onResize) {
            vnode.attrs.onResize(newPercent);
          }
        }
      };

      const onPointerUp = (e: PointerEvent) => {
        if (isResizing) {
          const handle = e.currentTarget as HTMLElement;
          handle.releasePointerCapture(e.pointerId);
          isResizing = false;
          document.body.style.cursor = '';
          document.body.style.userSelect = '';
          handle.classList.remove('pf-split-handle--active');
        }
      };

      return m('div', {class: containerClasses}, [
        m('.pf-split-panel__first', {style: firstStyle}, firstPanel),
        m('.pf-split-panel__handle', {
          onpointerdown: onPointerDown,
          onpointermove: onPointerMove,
          onpointerup: onPointerUp,
          onpointercancel: onPointerUp,
        }),
        m('.pf-split-panel__second', {style: secondStyle}, secondPanel),
      ]);
    },
  };
}
