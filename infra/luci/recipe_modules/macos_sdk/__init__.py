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
    '$perfetto/macos_sdk':
 Property(
        help='Properties specifically for the macos_sdk module.',
        param_name='sdk_properties',
        kind=ConfigGroup(  # pylint: disable=line-too-long
            # XCode build version number. Internally maps to an XCode build id like
            # '9c40b'. See
            #
            #   https://chrome-infra-packages.appspot.com/p/infra_internal/ios/xcode/mac/+/
            #
            # For an up to date list of the latest SDK builds.
            sdk_version=Single(str),

            # The CIPD toolchain tool package and version.
            tool_pkg=Single(str),
            tool_ver=Single(str),
        ),
        default={
            'sdk_version':
 '13F100',
            'tool_package':
 'infra/tools/mac_toolchain/${platform}',
            'tool_version':
 'git_revision:252677a648de0a12b7afec469a54830be659fb47',
        },
    )
}
