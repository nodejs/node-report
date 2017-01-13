// Main module entry point for node-report

const api = require('./api');

// NODE_REPORT_EVENTS env var overrides the defaults
const options = process.env.NODE_REPORT_EVENTS || 'exception+fatalerror+signal+apicall';
api.setEvents(options);

exports.triggerReport = api.triggerReport;
exports.setEvents = api.setEvents;
exports.setCoreDump = api.setCoreDump;
exports.setSignal = api.setSignal;
exports.setFileName = api.setFileName;
exports.setDirectory = api.setDirectory;
exports.setVerbose = api.setVerbose;
