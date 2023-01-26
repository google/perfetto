module.exports = {
  'env': {
    'browser': true,
    'es2021': true,
    'node': true,
  },
  'extends': [
    'google',
  ],
  'parser': '@typescript-eslint/parser',
  'parserOptions': {
    'ecmaVersion': 'latest',
    'sourceType': 'module',
  },
  'plugins': [
    '@typescript-eslint',
  ],
  'rules': {
    // We don't want to enforce jsdoc everywhere:
    'require-jsdoc': 'off',

    // Max line length is 80 with 2 space tabs. This must match the
    // ui/.clang-format definition:
    'max-len': [
      'error',
      {
        'code': 80,
        'tabWidth': 2,
        'ignoreUrls': true,
        'ignoreTemplateLiterals': true,
        'ignoreStrings': true,
      },
    ],

    // Indentation handled by clang-format --js:
    'indent': 'off',

    // clang-format --js formats EOL comments after (e.g.) an if like:
    // if (foo) {  // insightful comment
    // with two spaces between the slash and the brace. Turn
    // ignoreEOLComments on to allow that. We still want
    // no-multi-spaces turned on in general as it fixes issues like:
    // if (a ===   b)
    'no-multi-spaces': ['error', {ignoreEOLComments: true}],

    // Default no-unused-vars doesn't understand TypeScript enums. See:
    // https://github.com/typescript-eslint/typescript-eslint/issues/2621
    'no-unused-vars': 'off',
    '@typescript-eslint/no-unused-vars':
        ['error', {'argsIgnorePattern': '^_.*', 'varsIgnorePattern': '^_.*'}],

    // new Array() is banned (use [] instead) but new Array<Foo>() is
    // allowed since it can be clearer to put the type by the
    // construtor.
    'no-array-constructor': 'off',
    '@typescript-eslint/no-array-constructor': ['error'],

    // Rest parameters are not equivalent to 'arguments'.
    // Rest parameters are arrays: https://developer.mozilla.org/en-US/docs/Web/
    // JavaScript/Reference/Functions/rest_parameters
    // 'arguments' are objects: https://developer.mozilla.org/en-US/docs/Web/
    // JavaScript/Reference/Functions/arguments
    'prefer-rest-params': 'off',

    // We have a lot normal functions which are capitalised.
    // TODO(hjd): Switch these to be lowercase and remove capIsNew.
    // There are also some properties like: foo.factory these should
    // stay.
    'new-cap': ['error', {'capIsNew': false, 'properties': false}],
  },
};
