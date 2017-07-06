'use strict';

// Testcase to produce report on native crash (sigfpe)

// This testcase uses process.kill() to raise the sigfpe signal rather than
// producing a real arithmetic exception (which would require native code). The
// code exercised in node-report is the same.

if (process.argv[2] === 'child') {
  // Child process implementation
  require('../');
  process.kill(process.pid, 'SIGFPE');

} else {
  // Parent process implementation
  const common = require('./common.js');
  const spawn = require('child_process').spawn;
  const tap = require('tap');
  const fs = require('fs');

  if (common.isWindows()) {
    tap.fail('Native crash support not available on Windows', { skip: true });
    return;
  }

  const child = spawn(process.execPath, [__filename, 'child']);
  
  // Capture stderr output from the child process
  var stderr = '';
  child.stderr.on('data', (chunk) => {stderr += chunk;});

  // Validation on child exit
  child.on('exit', (code) => {
    tap.plan(4);
    tap.notEqual(code, 1, 'Check for expected non-zero exit code');
    const reports = common.findReports(child.pid);
    tap.equal(reports.length, 1, 'Found reports ' + reports);
    const report = reports[0];

    // Testcase-specific report validation
    fs.readFile(report, (err, data) => { 
      const headerSection = common.getSection(data, 'Node Report');
      tap.match(headerSection, /SIGFPE/,
                'Checking that header section contains crash signal name');
    });
    // Common report validation
    const options = {pid: child.pid, commandline: child.spawnargs.join(' ')};
    common.validate(tap, report, options);
  });
}
