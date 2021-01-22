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
"""Recipe for building Perfetto."""

from recipe_engine.recipe_api import Property

DEPS = [
    'recipe_engine/buildbucket',
    'recipe_engine/context',
    'recipe_engine/file',
    'recipe_engine/path',
    'recipe_engine/platform',
    'recipe_engine/properties',
    'recipe_engine/step',
]

PROPERTIES = {
    'repository':
        Property(
            kind=str,
            default='https://android.googlesource.com/platform/external/perfetto'
        ),
}

ARTIFACTS = ['trace_processor_shell']


def RunSteps(api, repository):
  builder_cache_dir = api.path['cache'].join('builder')
  src_dir = builder_cache_dir.join('perfetto')

  # Fetch the Perfetto repo.
  with api.step.nest('git'), api.context(infra_steps=True):
    api.file.ensure_directory('ensure source dir', src_dir)
    api.step('init', ['git', 'init', src_dir])
    with api.context(cwd=src_dir):
      build_input = api.buildbucket.build_input
      ref = (
          build_input.gitiles_commit.id
          if build_input.gitiles_commit else 'refs/heads/master')
      # Fetch tags so `git describe` works.
      api.step('fetch', ['git', 'fetch', '--tags', repository, ref])
      api.step('checkout', ['git', 'checkout', 'FETCH_HEAD'])

  # Pull all deps here.
  # There should be no need for internet access for building Perfetto beyond
  # this point.
  with api.context(cwd=src_dir, infra_steps=True):
    api.step('build-deps', ['tools/install-build-deps', '--ui', '--android'])

  # Buld Perfetto.
  with api.context(cwd=src_dir):
    api.step('gn gen', ['tools/gn', 'gen', 'out/dist', '--args=is_debug=false'])
    api.step('ninja', ['tools/ninja', '-C', 'out/dist'])


def GenTests(api):
  for platform in ('linux',):
    yield (api.test('ci_' + platform) + api.platform.name(platform) +
           api.buildbucket.ci_build(
               project='perfetto',
               git_repo='android.googlesource.com/platform/external/perfetto',
           ))
