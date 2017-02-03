// Example - generation of report via signal for a looping application
require('node-report').setEvents("signal");
var http = require("http");

var count = 0;

function my_listener(request, response) {
  switch(count++) {
  case 0:
    response.writeHead(200,{"Content-Type": "text/plain"});
    response.write("\nRunning node-report looping application demo. Node process ID = " + process.pid);
    response.write("\n\nRefresh page to enter loop, then use 'kill -USR2 " + process.pid + "' to trigger NodeReport");
    response.end();
    break;
  case 1:
    console.log("loop.js: going to loop now, use 'kill -USR2 " + process.pid + "' to trigger NodeReport");
    var list = [];
    for (var i=0; i<10000000000; i++) {
      for (var j=0; i<1000; i++) {
        list.push(new MyRecord());
      }
      for (var j=0; i<1000; i++) {
        list[j].id += 1;
        list[j].account += 2;
      }
      for (var j=0; i<1000; i++) {
        list.pop();
      }
    }
    response.writeHead(200,{"Content-Type": "text/plain"});
    response.write("\nnode-report demo.... finished looping");
    response.end();
    break;
  default:
  }
}

function MyRecord() {
  this.name = 'foo';
  this.id = 128;
  this.account = 98454324;
}

var http_server = http.createServer(my_listener);
http_server.listen(8080);

console.log('loop.js: Node running');
console.log('loop.js: Go to http://<machine>:8080/ or http://localhost:8080/');

setTimeout(function() {
  console.log('loop.js: timeout expired, exiting.');
  process.exit(0);
}, 60000);
