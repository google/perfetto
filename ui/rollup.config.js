import commonjs from 'rollup-plugin-commonjs';
import nodeResolve from 'rollup-plugin-node-resolve';

export default {
  moduleName: 'perfetto',
  plugins: [
    nodeResolve(),
    commonjs()
  ]
}
