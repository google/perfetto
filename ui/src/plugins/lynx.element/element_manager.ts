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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {LynxElement} from '../../lynx_perf/common_components/element_tree/types';

class ElementManager {
  private screenWidth: number = 0;
  private screenHeight: number = 0;
  private issueElementsMap: Map<number, LynxElement[]> = new Map(); // traceId -> LynxElement[]

  updateScreenSize(width: number, height: number) {
    this.screenWidth = width;
    this.screenHeight = height;
  }

  getScreenSize() {
    return {
      screenWidth: this.screenWidth,
      screenHeight: this.screenHeight,
    };
  }

  setTraceIssueElements(traceId: number, issueElements: LynxElement[]) {
    this.issueElementsMap.set(traceId, issueElements);
  }

  getTraceIssueElements(traceId: number): LynxElement[] | undefined {
    return this.issueElementsMap.get(traceId);
  }
}

export default new ElementManager();
