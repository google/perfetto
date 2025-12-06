from distutils.core import setup

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
    version='0.16.0',
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
