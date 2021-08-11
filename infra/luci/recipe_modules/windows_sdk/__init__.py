# Copyright (C) 2021 The Android Open Source Project
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

DEPS = [
    'recipe_engine/cipd',
    'recipe_engine/context',
    'recipe_engine/json',
    'recipe_engine/path',
    'recipe_engine/platform',
    'recipe_engine/step',
]

from recipe_engine.recipe_api import Property
from recipe_engine.config import ConfigGroup, Single

PROPERTIES = {
    '$perfetto/windows_sdk':
        Property(
            help='Properties specifically for the windows_sdk module.',
            param_name='sdk_properties',
            kind=ConfigGroup(
                # The CIPD package and version.
                sdk_package=Single(str),
                sdk_version=Single(str)),
            default={
                'sdk_package': 'chrome_internal/third_party/sdk/windows',
                'sdk_version': 'uploaded:2019-09-06'
            },
        )
}