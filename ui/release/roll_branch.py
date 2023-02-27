# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys
import json


def main():
  [_, channel_name, rev] = sys.argv

  with open('channels.json', 'r') as f:
    channels_json = json.load(f)

  found = False
  for channel in channels_json['channels']:
    if channel['name'] == channel_name:
      if channel['rev'] == rev:
        print(f'Channel {channel_name} is already at {rev}!')
        return
      channel['rev'] = rev
      found = True
      break

  if not found:
    print(f'Failed to find channel {channel_name}!')
    return

  with open('channels.json', 'w') as f:
    json.dump(channels_json, f, indent=2)
    # Right now channels.json ends with two line breaks,
    # keep it that way to minimise diffs.
    f.write('\n\n')


if __name__ == "__main__":
  main()
