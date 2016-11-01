// NodeReport API example
var nodereport = require('../');

console.log('api_call.js: triggering a NodeReport via API call...');

function myReport() {
  nodereport.triggerReport();
}

myReport();
