# nodereport

nodereport is an add-on for Node.js, delivered as an NPM native module,
which provides a human-readable diagnostic summary report, written
to file. The report is intended for development, test and production
use, to capture and preserve information for problem determination.
It includes Javascript and native stack traces, heap statistics,
platform information and resource usage etc. With the report enabled,
reports can be triggered on unhandled exceptions, fatal errors, signals
and calls to a Javascript API.

Usage:

    npm install nodejs/nodereport

    var nodereport = require('nodereport');

By default, this will allow a NodeReport to be triggered via an API
call from a JavaScript application. The filename of the NodeReport is
returned. The default filename includes the date, time, PID and a
sequence number. Alternatively a filename can be specified on the API call.

    nodereport.triggerReport();

    var filename = nodereport.triggerReport();

    nodereport.triggerReport("myReportName");

Content of the NodeReport in the initial implementation consists of a
header section containing the event type, date, time, PID and Node version,
sections containing Javascript and native stack traces, a section containing
V8 heap information, a section containing libuv handle information and an OS
platform information section showing CPU and memory usage and system limits.
The following messages are issued to stderr when a NodeReport is triggered:

    Writing Node.js error report to file: NodeReport.201605113.145311.26249.001.txt
    Node.js error report completed

A NodeReport can also be triggered on unhandled exception and fatal error
events, and/or signals (Linux/OSX only). These and other options can be 
enabled or disabled using the following APIs:

    nodereport.setEvents("exception+fatalerror+signal+apicall");
    nodereport.setSignal("SIGUSR2|SIGQUIT");
    nodereport.setFileName("stdout|stderr|<filename>");
    nodereport.setDirectory("<full path>");
    nodereport.setCoreDump("yes|no");
    nodereport.setVerbose("yes|no");

Configuration on module initialisation is also available via environment variables:

    export NODEREPORT_EVENTS=exception+fatalerror+signal+apicall
    export NODEREPORT_SIGNAL=SIGUSR2|SIGQUIT
    export NODEREPORT_FILENAME=stdout|stderr|<filename>
    export NODEREPORT_DIRECTORY=<full path>
    export NODEREPORT_COREDUMP=yes|no
    export NODEREPORT_VERBOSE=yes|no

Sample programs for triggering NodeReports are provided in the
node_modules/nodereport/demo directory:

    api.js - NodeReport triggered by Javascript API call
    exception.js - NodeReport triggered by unhandled exception
    fatalerror.js - NodeReport triggered by fatal error on Javascript heap out of memory
    loop.js - looping application, NodeReport triggered using kill -USR2 <pid>
