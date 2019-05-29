import commonjs from 'rollup-plugin-commonjs';
import nodeResolve from 'rollup-plugin-node-resolve';

export default {
  output: {name: 'perfetto'},
  plugins: [
    nodeResolve({module: false, browser: true}),

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
  ]
}
