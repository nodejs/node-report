'use strict';

// Testcase to produce report on fatal error (JavaScript heap OOM)
if (process.argv[2] === 'child') {
  require('../');

  const list = [];
  // Loop adding objects to the list until heap is full
  while (true) {
    const record = new MyRecord();
    list.push(record);
  }

  function MyRecord() {
    this.name = 'foo';
    this.buffer = Buffer.alloc(100000, 1);
  }

} else {
  // Testcase parent process implementation
  const common = require('./common.js');
  const spawn = require('child_process').spawn;
  const tap = require('tap');

  const args = ['--max-old-space-size=20', __filename, 'child'];
  const child = spawn(process.execPath, args);
  child.on('exit', (code) => {
    tap.plan(3);
    tap.notEqual(code, 0, 'Process should not exit cleanly');
    const reports = common.findReports(child.pid);
    tap.equal(reports.length, 1, 'Found reports ' + reports);
    const report = reports[0];
    const options = {pid: child.pid};
    // Node.js currently overwrites the command line on AIX
    // https://github.com/nodejs/node/issues/10607
    if (!(common.isAIX() || common.isSunOS())) {
      options.commandline = child.spawnargs.join(' ');
    }
    common.validate(tap, report, options);
  });
}
