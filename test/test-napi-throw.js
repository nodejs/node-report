'use strict';

const tap = require('tap');

// Build addon that throws an error
const cp = require('child_process');
const path = require('path');
const build = cp.spawnSync('npm', [ 'install', '.' ],
                           { cwd: path.join(__dirname, 'napi_throw'),
                           encoding: 'utf8' });
tap.equal(build.stderr, '', 'No errors building addon');
tap.equal(build.signal, null, 'Failed to build addon');
tap.equal(build.status, 0, 'Failed to build addon');

// Test catching the error does not trigger an UncaughtException report
const common = require('./common.js');
const report = require('../');
const obj = require('./napi_throw/build/Release/test_napi_throw.node');
tap.throws(obj.throwError);

const reports = common.findReports(process.pid);
tap.equals(reports, [], 'No reports should be generated');
