// Testcase for nodereport. Triggers and validates NodeReport files
// 
// Parameter(s): --save_log  (optional) preserves verbose log file
//
'use strict';

const assert = require('assert');
const spawn = require('child_process').spawn;
const readline = require('readline');
const fs = require('fs');
const path = require('path');

var results;
if (process.platform === 'win32') {
  results = ['fail','fail','fail'];
} else { 
  results = ['fail','fail','fail','fail'];
}

// Open a log file to record verbose test output
const stdio_log = fs.openSync('./nodereport_test.log', 'w');

// Test #1: Run child process to call API to generate a NodeReport
console.log('autorun.js: running child process #1 to produce NodeReport on API call');
var child1 = spawn(process.execPath, [path.resolve(__dirname, 'test_1.js')], {stdio: ['pipe', stdio_log, stdio_log]});
child1.on('exit', function(code, signal) {
  assert.ok(code == 0);
  // Locate and validate the NodeReport
  var report = locate(child1.pid);
  if (report) {
    validate(report, 1);
    fs.unlink(report); // delete the file when we are done
  }
});

// Test #2: Run child process to produce unhandled exception and generate a NodeReport
console.log('autorun.js: running child process #2 to produce NodeReport on exception');
var child2 = spawn(process.execPath, [path.resolve(__dirname, 'test_2.js')], {stdio: ['pipe', stdio_log, stdio_log]});
child2.on('exit', function(code, signal) {
  assert.ok(code !== 0);
  // Locate and validate the NodeReport
  var report = locate(child2.pid);
  if (report) {
    validate(report, 2);
    fs.unlink(report); // delete the file when we are done
  }
});

// Test #3: Run child process to produce fatal error (heap OOM) and generate a NodeReport
console.log('autorun.js: running child process #3 to produce NodeReport on fatal error');
var child3 = spawn(process.execPath, ['--max-old-space-size=20', path.resolve(__dirname, 'test_3.js')], {stdio: ['pipe', stdio_log, stdio_log]});
child3.on('exit', function(code, signal) {
  assert.ok(code !== 0);
  // Locate and validate the NodeReport
  var report = locate(child3.pid);
  if (report) {
    validate(report, 3);
    fs.unlink(report); // delete the file when we are done
  }
});

// Test #4: Run child process to loop, then send SIGUSR2 to generate a NodeReport
if (process.platform === 'win32') {
  // Not supported on Windows
} else {
  console.log('autorun.js: running child process #4 to produce NodeReport on SIGUSR2');
  var child4 = spawn(process.execPath, [path.resolve(__dirname, 'test_4.js')], {stdio: ['pipe', stdio_log, stdio_log]});
  setTimeout(function() {
      child4.kill('SIGUSR2');
      setTimeout(function() {
          child4.kill('SIGTERM');
      }, 1000);
  }, 1000);
  child4.on('exit', function(code, signal) {
    // Locate and validate the NodeReport
    var report = locate(child4.pid);
    if (report) {
      validate(report, 4);
      fs.unlink(report); // delete the file when we are done
    }
  });
}

// Print out results of all the tests on exit
process.on('exit', function() {
  console.log('autorun.js: test results: ');
  for (var i = 0; i < results.length; i++) {
    console.log('\ttest', i+1, ': ', results[i]);
  }
  // Close the verbose output log file, and delete it unless --save_log was specified
  fs.close(stdio_log);
  if (!(process.argv[2] === '--save_log')) {
    fs.unlink('./nodereport_test.log');
  }
});

// Utility function - locate a NodeReport produced by a process with the given PID
function locate(pid) {
  var files = fs.readdirSync(".");
  for (var i = 0; i < files.length; i++) {
    if (files[i].substring(0, 10) == 'NodeReport' && files[i].indexOf(pid) != -1) {
      console.log('autorun.js: located NodeReport: ', files[i]);
      return files[i];
    }
  }
  return;
}

// Utility function - validate a NodeReport (check all the sections are there) - and save result
function validate(report, index) {
  var validationCount = 0;
  const reader = readline.createInterface({
    input: fs.createReadStream(report)
  });
  reader.on('line', (line) => {
    //console.log('Line from file:', line);
    if ((line.indexOf('==== NodeReport') > -1) ||
        (line.indexOf('==== JavaScript Stack Trace') > -1) ||
        (line.indexOf('==== JavaScript Heap') > -1) ||
        (line.indexOf('==== System Information') > -1)) {
      validationCount++; // found expected section in NodeReport
    }
  });
  reader.on('close', () => {
    if (validationCount == 4) {
      results[index-1] = 'pass';
    } 
  });
}

