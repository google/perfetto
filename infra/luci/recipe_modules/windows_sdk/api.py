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

from contextlib import contextmanager

from recipe_engine import recipe_api


class WindowsSDKApi(recipe_api.RecipeApi):
  """API for using Windows SDK distributed via CIPD."""

  def __init__(self, sdk_properties, *args, **kwargs):
    super(WindowsSDKApi, self).__init__(*args, **kwargs)

    self._sdk_package = sdk_properties['sdk_package']
    self._sdk_version = sdk_properties['sdk_version']

  @contextmanager
  def __call__(self):
    """Setups the Windows SDK environment.

    This call is a no-op on non-Windows platforms.

    Raises:
        StepFailure or InfraFailure.
    """
    if not self.m.platform.is_win:
      yield
      return

    with self.m.context(infra_steps=True):
      sdk_dir = self._ensure_sdk()
    with self.m.context(**self._sdk_env(sdk_dir)):
      yield

  def _ensure_sdk(self):
    """Ensures the Windows SDK CIPD package is installed.

    Returns the directory where the SDK package has been installed.

    Args:
      path (path): Path to a directory.
      version (str): CIPD instance ID, tag or ref.
    """
    sdk_dir = self.m.path.cache_dir.joinpath('windows_sdk')
    pkgs = self.m.cipd.EnsureFile()
    pkgs.add_package(self._sdk_package, self._sdk_version)
    self.m.cipd.ensure(sdk_dir, pkgs)
    return sdk_dir

  def _sdk_env(self, sdk_dir):
    """Constructs the environment for the SDK.

    Returns environment and environment prefixes.

    Args:
      sdk_dir (path): Path to a directory containing the SDK.
    """
    env = {}
    env_prefixes = {}

    # Load .../win_sdk/bin/SetEnv.${arch}.json to extract the required
    # environment. It contains a dict that looks like this:
    # {
    #   "env": {
    #     "VAR": [["..", "..", "x"], ["..", "..", "y"]],
    #     ...
    #   }
    # }
    # All these environment variables need to be added to the environment
    # for the compiler and linker to work.
    filename = 'SetEnv.%s.json' % {32: 'x86', 64: 'x64'}[self.m.platform.bits]
    step_result = self.m.json.read(
        'read %s' % filename,
        sdk_dir / 'win_sdk' / 'bin' / filename,
        step_test_data=lambda: self.m.json.test_api.output({
            'env': {
                'PATH': [['..', '..', 'win_sdk', 'bin', 'x64']],
                'VSINSTALLDIR': [['..', '..\\']],
            },
        }))
    data = step_result.json.output.get('env')
    for key in data:
      # recipes' Path() does not like .., ., \, or /, so this is cumbersome.
      # What we want to do is:
      #   [sdk_bin_dir.join(*e) for e in env[k]]
      # Instead do that badly, and rely (but verify) on the fact that the paths
      # are all specified relative to the root, but specified relative to
      # win_sdk/bin (i.e. everything starts with "../../".)
      results = []
      for value in data[key]:
        assert value[0] == '..' and (value[1] == '..' or value[1] == '..\\')
        results.append('%s' % sdk_dir.joinpath(*value[2:]))

      # PATH is special-cased because we don't want to overwrite other things
      # like C:\Windows\System32. Others are replacements because prepending
      # doesn't necessarily makes sense, like VSINSTALLDIR.
      if key.lower() == 'path':
        env_prefixes[key] = results
      else:
        env[key] = ';'.join(results)

    return {'env': env, 'env_prefixes': env_prefixes}
