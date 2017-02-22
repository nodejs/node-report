'use strict';

// Testcase for validating reported OS version

const common = require('./common.js');
const nodereport = require('../');
const os = require('os');
const tap = require('tap');

const os_map = {
  'aix': 'AIX',
  'darwin': 'Darwin',
  'linux': 'Linux',
  'win32': 'Windows',
  'sunos': 'SunOS',
};
const os_name = os_map[os.platform()];
const report_str = nodereport.getReport();
const version_str = report_str.match(/OS version: .*(?:\r*\n)/);
if (common.isWindows()) {
  tap.match(version_str,
            new RegExp('OS version: ' + os_name), 'Checking OS version');
} else if (common.isAIX() && !os.release().includes('.')) {
  // For Node.js prior to os.release() fix for AIX:
  // https://github.com/nodejs/node/pull/10245
  tap.match(version_str,
            new RegExp('OS version: ' + os_name + ' \\d+.' + os.release()),
            'Checking OS version');
} else {
  tap.match(version_str,
            new RegExp('OS version: ' + os_name + ' .*' + os.release()),
            'Checking OS version');
}

