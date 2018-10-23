import commonjs from 'rollup-plugin-commonjs';
import nodeResolve from 'rollup-plugin-node-resolve';
import replace from 'rollup-plugin-replace';

export default {
  output: {name: 'perfetto'},
  plugins: [
    nodeResolve({browser: true}),

    // emscripten conditionally executes require('fs') (likewise for others),
    // when running under node. Rollup can't find those libraries so expects
    // these to be present in the global scope, which then fails at runtime.
    // To avoid this we ignore require('fs') and the like.
    commonjs({
      ignore: [
        'fs',
        'path',
        'crypto',
      ]
    }),

    replace({
      'immer_1.produce': 'immer_1',
    })

  ]
}
