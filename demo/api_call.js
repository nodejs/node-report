// Example - generation of report via API call
var nodereport = require('node-report');
var http = require("http");

var count = 0;

function my_listener(request, response) {
  switch(count++) {
  case 0:
    response.writeHead(200,{"Content-Type": "text/plain"});
    response.write("\nRunning node-report API demo... refresh page to trigger report");
    response.end();
    break;
  case 1:
    response.writeHead(200,{"Content-Type": "text/plain"});
    // Call the node-report module to trigger a report
    var filename = nodereport.triggerReport();
    response.write("\n" + filename + " written - refresh page to close");
    response.end();
    break;
  default:
    process.exit(0);
  }
}

var http_server = http.createServer(my_listener);
http_server.listen(8080);

console.log('api_call.js: Node running');
console.log('api_call.js: Go to http://<machine>:8080/ or http://localhost:8080/');

setTimeout(function(){
  console.log('api_call.js: test timeout expired, exiting.');
  process.exit(0);
}, 60000);
