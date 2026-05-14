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

import {InitialPageManager} from '../public/initial_page';

interface Suggestion {
  // Route to navigate to (e.g. '/heapdump').
  readonly route: string;
  // Higher wins.
  readonly priority: number;
  // Registration order, breaks ties at equal priority (earliest wins).
  readonly seq: number;
}

export class InitialPageManagerImpl implements InitialPageManager {
  private readonly suggestions = new Set<Suggestion>();
  private seqCounter = 0;

  suggest(route: string, priority: number): Disposable {
    const s: Suggestion = {route, priority, seq: this.seqCounter++};
    this.suggestions.add(s);
    return {[Symbol.dispose]: () => this.suggestions.delete(s)};
  }

  // Returns the winning route, or undefined if none was suggested.
  getWinner(): string | undefined {
    let best: Suggestion | undefined;
    for (const s of this.suggestions) {
      if (
        best === undefined ||
        s.priority > best.priority ||
        (s.priority === best.priority && s.seq < best.seq)
      ) {
        best = s;
      }
    }
    return best?.route;
  }
}
