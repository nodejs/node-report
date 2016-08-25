// NodeReport API example
var nodereport = require('nodereport');

console.log('api_call.js: triggering a NodeReport via API call...');

function myReport() {
    nodereport.triggerReport();
}

myReport();
