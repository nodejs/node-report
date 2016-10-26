# nodereport

nodereport is an add-on for Node.js, delivered as an NPM native module,
which provides a human-readable diagnostic summary report, written
to file. The report is intended for development, test and production
use, to capture and preserve information for problem determination.
It includes Javascript and native stack traces, heap statistics,
platform information and resource usage etc. With the report enabled,
reports can be triggered on unhandled exceptions, fatal errors, signals
and calls to a Javascript API. The module supports Node.js v4, v6 and v7
on Linux, MacOS and Windows.

Usage:

    npm install nodereport

By default, this will allow a NodeReport to be triggered via an API
call from a JavaScript application.

    var nodereport = require('nodereport');
    nodereport.triggerReport();

Content of the NodeReport consists of a header section containing the event
type, date, time, PID and Node version, sections containing Javascript and
native stack traces, a section containing V8 heap information, a section
containing libuv handle information and an OS platform information section
showing CPU and memory usage and system limits. An example NodeReport can be
triggered using the Node.js REPL:

    C:\test>node
    > nodereport = require('nodereport')
    > nodereport.triggerReport()
    Writing Node.js report to file: NodeReport.20161020.091102.8480.001.txt
    Node.js report completed
    >

When a NodeReport is triggered, start and end messages are issued to stderr
and the filename of the report is returned to the caller. The default filename
includes the date, time, PID and a sequence number. Alternatively, a filename
can be specified as a parameter on the triggerReport() call.

    nodereport.triggerReport("myReportName");

A NodeReport can also be triggered automatically on unhandled exceptions, fatal
error events (for example out of memory errors), and signals (Linux/OSX only).
Triggering on these events can be enabled using the following API call:

    nodereport.setEvents("exception+fatalerror+signal+apicall");

Additional configuration is available using the following APIs:


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

To see examples of NodeReports generated from these events you can run the
demonstration applications provided. These are Node.js applications which
will prompt you to access via a browser to trigger the required event.

    api.js - NodeReport triggered by Javascript API call
    exception.js - NodeReport triggered by unhandled exception
    fatalerror.js - NodeReport triggered by fatal error on Javascript heap out of memory
    loop.js - looping application, NodeReport triggered using kill -USR2 <pid>
