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

exports.triggerReport = function() {
  return api.triggerReport.apply(null, arguments);
};
exports.setEvents = function() {
  return api.setEvents.apply(null, arguments);
};
exports.setCoreDump = function() {
  return api.setCoreDump.apply(null, arguments);
};
exports.setSignal = function() {
  return api.setSignal.apply(null, arguments);
};
exports.setFileName = function() {
  return api.setFileName.apply(null, arguments);
};
exports.setDirectory = function() {
  return api.setDirectory.apply(null, arguments);
};
exports.setVerbose = function() {
  return api.setVerbose.apply(null, arguments);
};