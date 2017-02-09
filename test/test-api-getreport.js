'use strict';

// Testcase for returning NodeReport as a string via API call

const common = require('./common.js');
const tap = require('tap');
const nodereport = require('../');
var report_str = nodereport.getReport();
common.validateContent(report_str, tap, {pid: process.pid,
  commandline: [process.argv0, 'test/test-api-getreport.js'].join(' ')
});