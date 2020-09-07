from distutils.core import setup

setup(
    name='perfetto',
    packages=['perfetto', 'perfetto.trace_processor'],
    package_data={'perfetto.trace_processor': ['*.descriptor']},
    include_package_data=True,
    version='0.2.9',
    license='apache-2.0',
    description='Python API for Perfetto\'s Trace Processor',
    author='Perfetto',
    author_email='perfetto-pypi@google.com',
    url='https://perfetto.dev/',
    download_url='https://github.com/google/perfetto/archive/v6.0.tar.gz',
    keywords=['trace processor', 'tracing', 'perfetto'],
    install_requires=[
        'protobuf',
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
