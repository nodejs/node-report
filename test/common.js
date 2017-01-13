'use strict';

const fs = require('fs');

const REPORT_SECTIONS = [
  'node-report',
  'JavaScript Stack Trace',
  'JavaScript Heap',
  'System Information'
];

const reNewline = '(?:\\r*\\n)';

exports.findReports = (pid) => {
  // Default filenames are of the form node-report.<date>.<time>.<pid>.<seq>.txt
  const format = '^node-report\\.\\d+\\.\\d+\\.' + pid + '\\.\\d+\\.txt$';
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
      var plan = REPORT_SECTIONS.length + nodeComponents.length + 2;
      if (options.commandline) plan++;
      t.plan(plan);
      // Check all sections are present
      REPORT_SECTIONS.forEach((section) => {
        t.match(reportContents, new RegExp('==== ' + section),
                'Checking report contains ' + section + ' section');
      });

      // Check node-report header section
      const node-reportSection = getSection(reportContents, 'node-report');
      t.match(node-reportSection, new RegExp('Process ID: ' + pid),
              'node-report section contains expected process ID');
      if (options && options.expectNodeVersion === false) {
        t.match(node-reportSection, /Unable to determine Node.js version/,
                'node-report section contains expected Node.js version');
      } else {
        t.match(node-reportSection,
                new RegExp('Node.js version: ' + process.version),
                'node-report section contains expected Node.js version');
      }
      if (options && options.commandline) {
        if (this.isWindows()) {
          // On Windows we need to strip double quotes from the command line in
          // the report, and escape backslashes in the regex comparison string.
          t.match(node-reportSection.replace(/"/g,''),
                  new RegExp('Command line: '
                             + (options.commandline).replace(/\\/g,'\\\\')),
                  'Checking report contains expected command line');
        } else {
          t.match(node-reportSection,
                  new RegExp('Command line: ' + options.commandline),
                  'Checking report contains expected command line');
        }
      }
      nodeComponents.forEach((c) => {
        if (c !== 'node') {
          if (expectedVersions.indexOf(c) === -1) {
            t.notMatch(node-reportSection,
                       new RegExp(c + ': ' + process.versions[c]),
                       'node-report section does not contain ' + c + ' version');
          } else {
            t.match(node-reportSection,
                    new RegExp(c + ': ' + process.versions[c]),
                    'node-report section contains expected ' + c + ' version');
          }
        }
      });
      const node-reportMetadata = require('../package.json');
      t.match(node-reportSection,
              new RegExp('node-report version: ' + node-reportMetadata.version),
              'node-report section contains expected node-report version');
    });
  });
};

const getSection = (report, section) => {
  const re = new RegExp('==== ' + section + ' =+' + reNewline + '+([\\S\\s]+?)'
                        + reNewline + '+={80}' + reNewline);
  const match = re.exec(report);
  return match ? match[1] : '';
};
