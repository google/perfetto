import commonjs from 'rollup-plugin-commonjs';
import nodeResolve from 'rollup-plugin-node-resolve';
import replace from 'rollup-plugin-replace';

export default {
  output: {name: 'perfetto'},
  plugins: [
    nodeResolve({browser: true}),

    // emscripten conditionally executes require('fs') and require('path'),
    // when running under node, rollup can't find a library named 'fs' or
    // 'path' so expects these to be present in the global scope (which fails
    // at runtime). To avoid this we ignore require('fs') and require('path').
    commonjs({
      ignore: [
        'fs',
        'path',
      ]
    }),

    replace({
      'immer_1.produce': 'immer_1',
    })

  ]
}
