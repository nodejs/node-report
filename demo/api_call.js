// Example - generation of NodeReport via API call
var nodereport = require('nodereport');
var http = require("http");

var count = 0;

function my_listener(request, response) {
    switch(count++) {
    case 0:
        response.writeHead(200,{"Content-Type": "text/plain"});
        response.write("\nRunning NodeReport API demo... refresh page to trigger NodeReport");
        response.end();
        break;
    case 1:
        response.writeHead(200,{"Content-Type": "text/plain"});
        var filename = nodereport.triggerReport(); // Call the nodereport module to trigger a NodeReport 
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

