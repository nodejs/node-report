# nodereport

Delivers a human-readable diagnostic summary, written to file.

The report is intended for development, test and production
use, to capture and preserve information for problem determination.
It includes JavaScript and native stack traces, heap statistics,
platform information and resource usage etc. With the report enabled,
reports can be triggered on unhandled exceptions, fatal errors, signals
and calls to a JavaScript API.

Supports Node.js v4, v6 and v7 on Linux, MacOS, Windows and AIX.

## Usage

```bash
npm install node-report
node -r node-report app.js
```
A report will be triggered automatically on unhandled exceptions and fatal
error events (for example out of memory errors), and can also be triggered
by sending a USR2 signal to a Node.js process (AIX/Linux/MacOS only).

A report can also be triggered via an API call from a JavaScript
application.

```js
var nodereport = require('node-report');
nodereport.triggerReport();
```
The API can be used without adding the automatic exception and fatal error
hooks and the signal handler, as follows:

```js
var nodereport = require('node-report/api');
nodereport.triggerReport();
```

Content of the report consists of a header section containing the event
type, date, time, PID and Node version, sections containing JavaScript and
native stack traces, a section containing V8 heap information, a section
containing libuv handle information and an OS platform information section
showing CPU and memory usage and system limits. An example report can be
triggered using the Node.js REPL:

```
$ node
> nodereport = require('node-report')
> nodereport.triggerReport()
Writing Node.js report to file: node-report.20161020.091102.8480.001.txt
Node.js report completed
>
```

When a report is triggered, start and end messages are issued to stderr
and the filename of the report is returned to the caller. The default filename
includes the date, time, PID and a sequence number. Alternatively, a filename
can be specified as a parameter on the `triggerReport()` call.

```js
nodereport.triggerReport("myReportName");
```

## Configuration

Additional configuration is available using the following APIs:

```js
setEvents("exception+fatalerror+signal+apicall");
setSignal("SIGUSR2|SIGQUIT");
setFileName("stdout|stderr|<filename>");
setDirectory("<full path>");
setVerbose("yes|no");
```

Configuration on module initialization is also available via environment variables:

```bash
export NODEREPORT_EVENTS=exception+fatalerror+signal+apicall
export NODEREPORT_SIGNAL=SIGUSR2|SIGQUIT
export NODEREPORT_FILENAME=stdout|stderr|<filename>
export NODEREPORT_DIRECTORY=<full path>
export NODEREPORT_VERBOSE=yes|no
```

## Examples

To see examples of reports generated from these events you can run the
demonstration applications provided in the node-report github repository. These are
Node.js applications which will prompt you to trigger the required event.

1. `api.js` - report triggered by JavaScript API call.
2. `exception.js` - report triggered by unhandled exception.
3. `fatalerror.js` - report triggered by fatal error on JavaScript heap out of memory.
4. `loop.js` - looping application, report triggered using kill `-USR2 <pid>`.

## License

[Licensed under the MIT License.](LICENSE.md)
