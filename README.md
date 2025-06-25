# Abort lynx-trace

**lynx-trace** is a performance analysis tool for [Lynx](https://github.com/lynx-family/lynx). Built on the open-source [google/perfetto](https://github.com/google/perfetto), **lynx-trace** incorporates the principles of Lynx to provide multiple analysis capabilities, helping developers optimize the performance of Lynx pages.

## Features
- **Lynx Rendering Pipeline Visualization**: Helps developers understand and analyze the rendering pipelines of Lynx pages.
- **Detailed Execution Flow Visualization**: Helps developers understand the principles of Lynx.
- **Comprehensive Analysis Plugins**: Offers a suite of plugins including DOM analysis, NativeModules call analysis, and Timing analysis to help developers pinpoint, analyze, and resolve performance issues on Lynx pages.

# How to use

## Use Lynx Devtool
Lynx Devtool has integrated **lynx-trace**, and can be used directly within Lynx Devtool. See the [documentation](https://lynxjs.org/zh/guide/debugging/lynx-devtool.html) for more information.

## Build Trace Web from Source
1. Clone repository and switch to development branch:
   ```sh
   git clone https://github.com/lynx/lynx-trace.git
   cd lynx-trace
   ```
2. Install dependencies:
   ```sh
   ./tools/install-build-deps --ui
   ```
3. Build the project:
   ```sh
   ./ui/build --serve
   ```
4. Open your browser and navigate to http://localhost:10000/
5. Drag and drop the trace file into the page to analyze it.

# Contributing
We welcome contributions from the community. To get started:
1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Make your changes and commit them.
4. Push your changes to your fork.
5. Submit a pull request to the main repository.

# License
This project is licensed under the Apache 2.0 License. See the [LICENSE](LICENSE) file for details.

# Acknowledgments
We would like to thank the contributors of [google/perfetto](https://github.com/google/perfetto) for their foundational work that enables lynx-trace.