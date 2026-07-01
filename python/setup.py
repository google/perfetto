import os
import re

from setuptools import setup


def _version_from_changelog():
  """Derives the PyPI package version, e.g. '0.56.0'.

  Normally read from the top 'vX.Y' entry of the repo CHANGELOG, mapped to
  '0.X.Y'. The CHANGELOG lives outside the package so it is not in the sdist;
  when building from an sdist the CHANGELOG is absent, so fall back to the
  version setuptools wrote into PKG-INFO when the sdist was created.
  """
  here = os.path.dirname(os.path.abspath(__file__))
  changelog = os.path.join(here, os.pardir, 'CHANGELOG')
  if os.path.exists(changelog):
    with open(changelog) as f:
      for line in f:
        m = re.match(r'^v(\d+)[.](\d+)\s', line)
        if m:
          return '0.%s.%s' % (m.group(1), m.group(2))
    raise RuntimeError('No vX.Y entry found in %s' % changelog)

  pkg_info = os.path.join(here, 'PKG-INFO')
  if os.path.exists(pkg_info):
    with open(pkg_info) as f:
      for line in f:
        if line.startswith('Version:'):
          return line.split(':', 1)[1].strip()
  raise RuntimeError('Cannot determine version: no CHANGELOG or PKG-INFO')


setup(
    name='perfetto',
    packages=[
        'perfetto',
        'perfetto.batch_trace_processor',
        'perfetto.common',
        'perfetto.prebuilts',
        'perfetto.prebuilts.manifests',
        'perfetto.protos.perfetto.trace',
        'perfetto.trace_builder',
        'perfetto.trace_processor',
        'perfetto.trace_uri_resolver',
    ],
    package_data={
        'perfetto.trace_processor': ['*.descriptor'],
    },
    include_package_data=True,
    version=_version_from_changelog(),
    license='apache-2.0',
    description='Python APIs and bindings for Perfetto (perfetto.dev)',
    author='Perfetto',
    author_email='perfetto-pypi@google.com',
    url='https://perfetto.dev/',
    download_url='https://github.com/google/perfetto/archive/a760e3fc2f84d84225bfb4928d281c4b7c51d193.zip',
    keywords=['trace processor', 'tracing', 'perfetto'],
    install_requires=[
        'protobuf',
    ],
    extras_require={
        'numpy': ['numpy'],
        'pandas': ['pandas'],
        'polars': ['polars'],
    },
    classifiers=[
        'Development Status :: 3 - Alpha',
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
    ],
)
