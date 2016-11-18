'use strict';

const fs = require('fs');

const REPORT_SECTIONS = [
  'NodeReport',
  'JavaScript Stack Trace',
  'JavaScript Heap',
  'System Information'
];

exports.findReports = (pid) => {
  // Default filenames are of the form NodeReport.<date>.<time>.<pid>.<seq>.txt
  const format = '^NodeReport\\.\\d+\\.\\d+\\.' + pid + '\\.\\d+\\.txt$';
  const filePattern = new RegExp(format);
  const files = fs.readdirSync('.');
  return files.filter((file) => filePattern.test(file));
};

exports.isPPC = () => {
  return process.arch.startsWith('ppc');
};

exports.isWindows = () => {
  return process.platform === 'win32';
};

exports.validate = (t, report, pid) => {
  t.test('Validating ' + report, (t) => {
    fs.readFile(report, (err, data) => {
      const reportContents = data.toString();
      const plan = REPORT_SECTIONS.length + (pid ? 1 : 0);
      t.plan(plan);
      if (pid) {
        t.match(reportContents, new RegExp('Process ID: ' + pid),
                'Checking report contains expected process ID ' + pid);
      }
      REPORT_SECTIONS.forEach((section) => {
        t.match(reportContents, new RegExp('==== ' + section),
                'Checking report contains ' + section + ' section');
      });
    });
  });
};
