// Copyright (C) 2023 The Android Open Source Project
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

import {Command} from '../public';

export interface CommandSource {
  commands(): Command[];
}

export class CommandManager {
  private commandSources: CommandSource[] = [];

  registerCommandSource(cs: CommandSource) {
    this.commandSources.push(cs);
  }

  get commands(): Command[] {
    return this.commandSources.flatMap((source) => source.commands());
  }

  runCommand(id: string, ...args: any[]): void {
    const cmd = this.commands.find((cmd) => cmd.id === id);
    if (cmd) {
      cmd.callback(...args);
    } else {
      console.error(`No such command: ${id}`);
    }
  }

  // TODO(stevegolton): Improve filter algo.
  fuzzyFilterCommands(searchTerm: string): Command[] {
    const searchTermLower = searchTerm.toLowerCase();
    return this.commands.filter(({name}) => {
      return name.toLowerCase().includes(searchTermLower);
    });
  }
}
