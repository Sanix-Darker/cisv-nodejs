const path = require('path');
const addon = require('node-gyp-build')(path.join(__dirname, '..'));
const { wrapAddon } = require('./wrapper');

module.exports = wrapAddon(addon);
