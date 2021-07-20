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
    'depot_tools/gsutil',
    'recipe_engine/buildbucket',
    'recipe_engine/context',
    'recipe_engine/file',
    'recipe_engine/path',
    'recipe_engine/platform',
    'recipe_engine/properties',
    'recipe_engine/raw_io',
    'recipe_engine/step',
    'macos_sdk',
    'windows_sdk',
]

PROPERTIES = {
    'repository':
        Property(
            kind=str,
            default='https://android.googlesource.com/platform/external/perfetto'
        ),
}

ARTIFACTS = [
    {
        'name': 'trace_processor_shell'
    },
    {
        'name':
            'trace_to_text',
        'exclude_platforms': [
            'android-arm', 'android-arm64', 'android-x86', 'android-x64'
        ]
    },
    {
        'name': 'tracebox',
        'exclude_platforms': ['windows-amd64']
    },
    {
        'name': 'perfetto'
    },
    {
        'name': 'traced'
    },
    {
        'name': 'traced_probes',
        'exclude_platforms': ['windows-amd64']
    },
]


def GnArgs(platform):
  (os, cpu) = platform.split('-')

  base_args = 'is_debug=false monolithic_binaries=true'
  if 'android' != os:
    return base_args
  return base_args + ' target_os="{}" target_cpu="{}"'.format(os, cpu)


def UploadArtifact(api, platform, upload_dir, artifact):
  exclude_platforms = artifact.get('exclude_platforms', [])
  if platform in exclude_platforms:
    return

  exe_path = 'out/dist' if api.platform.is_win else 'out/dist/stripped'
  artifact_ext = artifact['name'] + ('.exe' if api.platform.is_win else '')
  source = '{}/{}'.format(exe_path, artifact_ext)
  target = '{}/{}/{}'.format(upload_dir, platform, artifact_ext)
  api.gsutil.upload(source, 'perfetto-luci-artifacts', target)


def BuildForPlatform(api, platform, src_dir, upload_dir):
  # Buld Perfetto.
  # There should be no need for internet access here.
  with api.context(cwd=src_dir), api.macos_sdk(), api.windows_sdk():
    args = GnArgs(platform)
    api.step('gn gen', [
        'python3', 'tools/gn', 'gen', 'out/{}'.format(platform),
        '--args={}'.format(args)
    ])
    api.step('ninja',
             ['python3', 'tools/ninja', '-C', 'out/{}'.format(platform)])

  # Upload stripped artifacts using gsutil if we're on the official builder.
  if 'official' not in api.buildbucket.builder_id.builder:
    return

  with api.step.nest('Artifact upload'), api.context(cwd=src_dir):
    for artifact in ARTIFACTS:
      UploadArtifact(api, platform, upload_dir, artifact)


def RunSteps(api, repository):
  # The directory for any uploaded artifacts. This will be the tag name
  # (if building a tag) or the SHA of the commit otherwise.
  upload_dir = None

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

      if ref.startswith('refs/tags/'):
        upload_dir = ref.replace('refs/tags/', '')
      else:
        upload_dir = api.step(
            'rev-parse', ['git', 'rev-parse', 'HEAD'],
            stdout=api.raw_io.output()).stdout.strip()

  # Pull all deps here.
  with api.context(cwd=src_dir, infra_steps=True):
    extra_args = ['--android'
                 ] if 'android' in api.buildbucket.builder_id.builder else []
    api.step('build-deps', ['python3', 'tools/install-build-deps'] + extra_args)

  if api.platform.is_win:
    BuildForPlatform(api, 'windows-amd64', src_dir, upload_dir)
  elif api.platform.is_mac:
    BuildForPlatform(api, 'mac-amd64', src_dir, upload_dir)
  elif 'android' in api.buildbucket.builder_id.builder:
    with api.step.nest('android-arm'):
      BuildForPlatform(api, 'android-arm', src_dir, upload_dir)
    with api.step.nest('android-arm64'):
      BuildForPlatform(api, 'android-arm64', src_dir, upload_dir)
    with api.step.nest('android-x86'):
      BuildForPlatform(api, 'android-x86', src_dir, upload_dir)
    with api.step.nest('android-x64'):
      BuildForPlatform(api, 'android-x64', src_dir, upload_dir)
  else:
    BuildForPlatform(api, 'linux-amd64', src_dir, upload_dir)


def GenTests(api):
  for platform in ('linux', 'mac', 'win'):
    yield (api.test('ci_' + platform) + api.platform.name(platform) +
           api.buildbucket.ci_build(
               project='perfetto',
               builder='official',
               git_repo='android.googlesource.com/platform/external/perfetto',
           ))

  yield (api.test('ci_tag') + api.platform.name('linux') +
         api.buildbucket.ci_build(
             project='perfetto',
             builder='official',
             git_repo='android.googlesource.com/platform/external/perfetto',
             revision='refs/tags/v13.0'))

  yield (api.test('ci_android') + api.platform.name('linux') +
         api.buildbucket.ci_build(
             project='perfetto',
             builder='official-android',
             git_repo='android.googlesource.com/platform/external/perfetto'))

  yield (api.test('unofficial') + api.platform.name('linux') +
         api.buildbucket.ci_build(
             project='perfetto',
             git_repo='android.googlesource.com/platform/external/perfetto'))
