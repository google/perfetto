import os
import re

from distutils.core import setup


def _version_from_changelog():
  """Derives the PyPI package version from the top entry of CHANGELOG.

  The CHANGELOG uses entries like 'vX.Y - YYYY-MM-DD:' for released versions
  (and 'Unreleased:' at the top while a release is in flight). The first
  matching 'vX.Y' line is mapped to the PyPI version '0.X.Y' — keeping the
  package in the 0.x series while encoding the Perfetto release in the
  minor/patch components.
  """
  changelog = os.path.join(
      os.path.dirname(os.path.abspath(__file__)), os.pardir, 'CHANGELOG')
  with open(changelog) as f:
    for line in f:
      m = re.match(r'^v(\d+)[.](\d+)\s', line)
      if m:
        return '0.%s.%s' % (m.group(1), m.group(2))
  raise RuntimeError('No vX.Y entry found in %s' % changelog)


setup(
    name='perfetto',
    packages=[
        'perfetto',
        'perfetto.batch_trace_processor',
        'perfetto.common',
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
    download_url='https://github.com/google/perfetto/archive/bb5f4f019e2a1b5bc6e4c8203f05890d96467cf7.zip',
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
