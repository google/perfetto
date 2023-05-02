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
    'recipe_engine/cipd',
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
        'name': 'traceconv',
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


class BuildContext:

  def __init__(self, src_dir):
    self.src_dir = src_dir
    self.git_revision = None
    self.maybe_git_tag = None


def GnArgs(platform):
  (os, cpu) = platform.split('-')
  base_args = 'is_debug=false monolithic_binaries=true'
  if os not in ('android', 'linux', 'mac'):
    return base_args  # No cross-compiling on Windows.
  cpu = 'x64' if cpu == 'amd64' else cpu  # GN calls it "x64".
  return base_args + ' target_os="{}" target_cpu="{}"'.format(os, cpu)


def UploadArtifact(api, ctx, platform, out_dir, artifact):
  exclude_platforms = artifact.get('exclude_platforms', [])
  if platform in exclude_platforms:
    return

  # We want to use the stripped binaries except on Windows where we don't generate
  # them.
  exe_dir = out_dir if api.platform.is_win else out_dir.join('stripped')

  # Compute the exact artifact path
  gcs_upload_dir = ctx.maybe_git_tag if ctx.maybe_git_tag else ctx.git_revision
  artifact_ext = artifact['name'] + ('.exe' if api.platform.is_win else '')
  source_path = exe_dir.join(artifact_ext)

  # Upload to GCS bucket.
  gcs_target_path = '{}/{}/{}'.format(gcs_upload_dir, platform, artifact_ext)
  api.gsutil.upload(source_path, 'perfetto-luci-artifacts', gcs_target_path)

  # Create the CIPD package definition from the artifact path.
  cipd_pkg_name = 'perfetto/{}/{}'.format(artifact['name'], platform)
  pkg_def = api.cipd.PackageDefinition(
      package_name=cipd_pkg_name, package_root=exe_dir)
  pkg_def.add_file(source_path)

  # Actually build the CIPD pakcage
  cipd_pkg_file_name = '{}-{}.cipd'.format(artifact['name'], platform)
  cipd_pkg_file = api.path['cleanup'].join(cipd_pkg_file_name)
  api.cipd.build_from_pkg(
      pkg_def=pkg_def,
      output_package=cipd_pkg_file,
  )

  # If we have a git tag, add that to the CIPD tags.
  tags = {
      'git_revision': ctx.git_revision,
  }
  if ctx.maybe_git_tag:
    tags['git_tag'] = ctx.maybe_git_tag

  # Upload the package and regisiter with the 'latest' tag.
  api.cipd.register(
      package_name=cipd_pkg_name,
      package_path=cipd_pkg_file,
      refs=['latest'],
      tags=tags,
  )


def BuildForPlatform(api, ctx, platform):
  out_dir = ctx.src_dir.join('out', platform)

  # Build Perfetto.
  # There should be no need for internet access here.

  with api.context(cwd=ctx.src_dir), api.macos_sdk(), api.windows_sdk():
    targets = [
        x['name']
        for x in ARTIFACTS
        if platform not in x.get('exclude_platforms', [])
    ]
    args = GnArgs(platform)
    api.step('gn gen',
             ['python3', 'tools/gn', 'gen', out_dir, '--args={}'.format(args)])
    api.step('ninja', ['python3', 'tools/ninja', '-C', out_dir] + targets)

  # Upload stripped artifacts using gsutil if we're on the official builder.
  if 'official' not in api.buildbucket.builder_id.builder:
    return

  with api.step.nest('Artifact upload'), api.context(cwd=ctx.src_dir):
    for artifact in ARTIFACTS:
      UploadArtifact(api, ctx, platform, out_dir, artifact)


def RunSteps(api, repository):
  builder_cache_dir = api.path['cache'].join('builder')
  src_dir = builder_cache_dir.join('perfetto')

  # Crate the context we use in all the building stages.
  ctx = BuildContext(src_dir)

  # Fetch the Perfetto repo.
  with api.step.nest('git'), api.context(infra_steps=True):
    api.file.ensure_directory('ensure source dir', src_dir)
    api.step('init', ['git', 'init', src_dir])
    with api.context(cwd=src_dir):
      build_input = api.buildbucket.build_input
      ref = (
          build_input.gitiles_commit.ref
          if build_input.gitiles_commit else 'refs/heads/master')
      # Fetch tags so `git describe` works.
      api.step('fetch', ['git', 'fetch', '--tags', repository, ref])
      api.step('checkout', ['git', 'checkout', 'FETCH_HEAD'])

      # Store information about the git revision and the tag if available.
      ctx.git_revision = api.step(
          'rev-parse', ['git', 'rev-parse', 'HEAD'],
          stdout=api.raw_io.output_text()).stdout.strip()
      ctx.maybe_git_tag = ref.replace(
          'refs/tags/', '') if ref.startswith('refs/tags/') else None

  # Pull all deps here.
  with api.context(cwd=src_dir, infra_steps=True):
    extra_args = []
    if 'android' in api.buildbucket.builder_id.builder:
      extra_args += ['--android']
    elif api.platform.is_linux:
      # Pull the cross-toolchains for building for linux-arm{,64}.
      extra_args += ['--linux-arm']
    api.step('build-deps', ['python3', 'tools/install-build-deps'] + extra_args)

  if api.platform.is_win:
    BuildForPlatform(api, ctx, 'windows-amd64')
  elif api.platform.is_mac:
    with api.step.nest('mac-amd64'):
      BuildForPlatform(api, ctx, 'mac-amd64')
    with api.step.nest('mac-arm64'):
      BuildForPlatform(api, ctx, 'mac-arm64')
  elif 'android' in api.buildbucket.builder_id.builder:
    with api.step.nest('android-arm'):
      BuildForPlatform(api, ctx, 'android-arm')
    with api.step.nest('android-arm64'):
      BuildForPlatform(api, ctx, 'android-arm64')
    with api.step.nest('android-x86'):
      BuildForPlatform(api, ctx, 'android-x86')
    with api.step.nest('android-x64'):
      BuildForPlatform(api, ctx, 'android-x64')
  elif api.platform.is_linux:
    with api.step.nest('linux-amd64'):
      BuildForPlatform(api, ctx, 'linux-amd64')
    with api.step.nest('linux-arm'):
      BuildForPlatform(api, ctx, 'linux-arm')
    with api.step.nest('linux-arm64'):
      BuildForPlatform(api, ctx, 'linux-arm64')


def GenTests(api):
  for target in ('android', 'linux', 'mac', 'win'):
    host = 'linux' if target == 'android' else target
    yield (api.test('ci_' + target) + api.platform.name(host) +
           api.buildbucket.ci_build(
               project='perfetto',
               builder='perfetto-official-builder-%s' % target,
               git_repo='android.googlesource.com/platform/external/perfetto',
           ))

  yield (api.test('ci_tag') + api.platform.name('linux') +
         api.buildbucket.ci_build(
             project='perfetto',
             builder='official',
             git_repo='android.googlesource.com/platform/external/perfetto',
             git_ref='refs/tags/v13.0'))

  yield (api.test('unofficial') + api.platform.name('linux') +
         api.buildbucket.ci_build(
             project='perfetto',
             git_repo='android.googlesource.com/platform/external/perfetto'))
