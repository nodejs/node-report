'use strict';

const fs = require('fs');

const REPORT_SECTIONS = [
  'NodeReport',
  'JavaScript Stack Trace',
  'JavaScript Heap',
  'System Information'
];

const reNewline = '(?:\\r*\\n)';

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

exports.validate = (t, report, options) => {
  t.test('Validating ' + report, (t) => {
    fs.readFile(report, (err, data) => {
      const pid = options ? options.pid : process.pid;
      const reportContents = data.toString();
      const nodeComponents = Object.keys(process.versions);
      const expectedVersions = options ?
                               options.expectedVersions || nodeComponents :
                               nodeComponents;
      const plan = REPORT_SECTIONS.length + nodeComponents.length + 2;
      t.plan(plan);

      // Check all sections are present
      REPORT_SECTIONS.forEach((section) => {
        t.match(reportContents, new RegExp('==== ' + section),
                'Checking report contains ' + section + ' section');
      });

      // Check NodeReport section
      const nodeReportSection = getSection(reportContents, 'NodeReport');
      t.match(nodeReportSection, new RegExp('Process ID: ' + pid),
              'NodeReport section contains expected process ID');
      if (options && options.expectNodeVersion === false) {
        t.match(nodeReportSection, /Unable to determine Node.js version/,
                'NodeReport section contains expected Node.js version');
      } else {
        t.match(nodeReportSection,
                new RegExp('Node.js version: ' + process.version),
                'NodeReport section contains expected Node.js version');
      }
      nodeComponents.forEach((c) => {
        if (c !== 'node') {
          if (expectedVersions.indexOf(c) === -1) {
            t.notMatch(nodeReportSection,
                       new RegExp(c + ': ' + process.versions[c]),
                       'NodeReport section does not contain ' + c + ' version');
          } else {
            t.match(nodeReportSection,
                    new RegExp(c + ': ' + process.versions[c]),
                    'NodeReport section contains expected ' + c + ' version');
          }
        }
      });
      const nodereportMetadata = require('../package.json');
      t.match(nodeReportSection,
              new RegExp('NodeReport version: ' + nodereportMetadata.version),
              'NodeReport section contains expected NodeReport version');
    });
  });
};

const getSection = (report, section) => {
  const re = new RegExp('==== ' + section + ' =+' + reNewline + '+([\\S\\s]+?)'
                        + reNewline + '+={80}' + reNewline);
  const match = re.exec(report);
  return match ? match[1] : '';
};
