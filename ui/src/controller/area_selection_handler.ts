// Copyright (C) 2021 The Android Open Source Project
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

import {Area, AreaById} from '../common/state';
import {globals as frontendGlobals} from '../frontend/globals';

export class AreaSelectionHandler {
  private previousArea?: Area;

  getAreaChange(): [boolean, AreaById|undefined] {
    const currentSelection = frontendGlobals.state.currentSelection;
    if (currentSelection === null || currentSelection.kind !== 'AREA') {
      return [false, undefined];
    }

    const selectedArea = frontendGlobals.state.areas[currentSelection.areaId];
    // Area is considered changed if:
    // 1. The new area is defined and the old area undefined.
    // 2. The new area is undefined and the old area defined (viceversa from 1).
    // 3. Both areas are defined but their start or end times differ.
    // 4. Both areas are defined but their tracks differ.
    let hasAreaChanged = (!!this.previousArea !== !!selectedArea);
    if (selectedArea && this.previousArea) {
      // There seems to be an issue with clang-format http://shortn/_Pt98d5MCjG
      // where `a ||= b` is formatted to `a || = b`, by inserting a space which
      // breaks the operator.
      // Therefore, we are using the pattern `a = a || b` instead.
      hasAreaChanged = hasAreaChanged ||
          selectedArea.startSec !== this.previousArea.startSec;
      hasAreaChanged =
          hasAreaChanged || selectedArea.endSec !== this.previousArea.endSec;
      hasAreaChanged = hasAreaChanged ||
          selectedArea.tracks.length !== this.previousArea.tracks.length;
      for (let i = 0; i < selectedArea.tracks.length; ++i) {
        hasAreaChanged = hasAreaChanged ||
            selectedArea.tracks[i] !== this.previousArea.tracks[i];
      }
    }

    if (hasAreaChanged) {
      this.previousArea = selectedArea;
    }

    return [hasAreaChanged, selectedArea];
  }
}
