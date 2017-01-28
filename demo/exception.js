// Example - generation of NodeReport on uncaught exception
'use strict';

require('nodereport').setEvents('exception');
var http = require('http');

var count = 0;

function my_listener(request, response) {
  switch (count++) {
    case 0:
      response.writeHead(200, {'Content-Type': 'text/plain'});
      response.write('\nRunning NodeReport exception demo... refresh page to ' +
        'cause exception (application will terminate)');
      response.end();
      break;
    default:
      throw new UserException('*** exception.js: exception thrown from ' +
        'my_listener()');
  }
}

function UserException(message) {
  this.message = message;
  this.name = 'UserException';
}

var http_server = http.createServer(my_listener);
http_server.listen(8080);

console.log('exception.js: Node running');
console.log('exception.js: Go to http://<machine>:8080/ or ' +
  'http://localhost:8080/');

setTimeout(function() {
  console.log('exception.js: test timeout expired, exiting.');
  process.exit(0);
}, 60000);
