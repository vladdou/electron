'use strict'

const { deprecate } = require('electron')

if (!process.noDeprecations) {
  deprecate.warn(`require('fs')`, `remote.require('fs')`)
}

const { getRemoteForUsage } = require('@electron/internal/renderer/remote')
module.exports = getRemoteForUsage('fs').require('fs')
