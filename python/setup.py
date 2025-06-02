from distutils.core import setup

setup(
    name='perfetto',
    packages=[
        'perfetto',
        'perfetto.batch_trace_processor',
        'perfetto.common',
        'perfetto.trace_processor',
        'perfetto.trace_uri_resolver',
        'perfetto.bigtrace',
        'perfetto.bigtrace.protos',
    ],
    package_data={
        'perfetto.trace_processor': ['*.descriptor'],
        'perfetto.bigtrace.protos': ['**/*.py', '**/*.pyi']
    },
    include_package_data=True,
    version='0.12.0',
    license='apache-2.0',
    description='Python APIs and bindings for Perfetto (perfetto.dev)',
    author='Perfetto',
    author_email='perfetto-pypi@google.com',
    url='https://perfetto.dev/',
    download_url='https://github.com/google/perfetto/archive/aeddce7011258bffbb8a870923202db97c34e655.zip',
    keywords=['trace processor', 'tracing', 'perfetto'],
    install_requires=[
        'protobuf',
    ],
    extras_requires=[
        'numpy',
        'pandas',
    ],
    classifiers=[
        'Development Status :: 3 - Alpha',
        'License :: OSI Approved :: Apache Software License',
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
    ],
)
