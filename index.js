// Main module entry point for nodereport

const api = require('./api');

// Process NODEREPORT_EVENTS env var
const options = process.env.NODEREPORT_EVENTS;
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
