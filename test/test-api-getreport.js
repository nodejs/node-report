'use strict';

// Testcase for returning report as a string via API call

const common = require('./common.js');
const tap = require('tap');
const nodereport = require('../');
var report_str = nodereport.getReport();
common.validateContent(report_str, tap, {pid: process.pid,
  commandline: [process.argv[0], 'test/test-api-getreport.js'].join(' ')
});
