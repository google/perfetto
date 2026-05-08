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

import m from 'mithril';
import {classNames} from '../../../base/classnames';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {PopupPosition} from '../../../widgets/popup';

/**
 * Wraps `content` with a truncation warning overlay when the chart is
 * displaying fewer items than the total available.
 *
 * Only runs the comparison (and wraps) when `showWarning` is true and
 * `total` is present — callers should gate the `shown` computation similarly.
 */
export function maybeWrapWithTruncation(
  content: m.Child,
  shownCount: number | undefined,
  totalCount: number | undefined,
  showWarning?: boolean,
): m.Child {
  if (
    !showWarning ||
    totalCount === undefined ||
    shownCount === undefined ||
    totalCount <= shownCount
  ) {
    return content;
  }
  return m('.pf-chart-truncation-wrapper', [
    content,
    m(
      Tooltip,
      {
        trigger: m(Icon, {
          className: classNames('pf-chart-truncation-warning'),
          icon: 'warning',
          filled: true,
        }),
        position: PopupPosition.BottomEnd,
        showArrow: false,
      },
      `Showing ${shownCount} of ${totalCount} items`,
    ),
  ]);
}
