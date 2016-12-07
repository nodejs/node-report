// Main module entry point for nodereport

var api = require('./api');

// Process NODEREPORT_EVENTS env var
var options = process.env.NODEREPORT_EVENTS;
if (options) {
  api.setEvents(options);
} else {
  // Default action - all events enabled
  api.setEvents('exception+fatalerror+signal+apicall');
}

exports.triggerReport = api.triggerReport;
exports.setEvents = api.setEvents;
exports.setCoreDump = api.setCoreDump;
exports.setSignal = api.setSignal;
exports.setFileName = api.setFileName;
exports.setDirectory = api.setDirectory;
exports.setVerbose = api.setVerbose;
