// Example - generation of Nodereport on fatal error (Javascript heap OOM)
require('nodereport').setEvents("fatalerror");
var http = require('http');

var count = 0;

function my_listener(request, response) {
  switch(count++) {
  case 0:
    response.writeHead(200,{"Content-Type": "text/plain"});
    response.write("\nRunning NodeReport fatal error demo... refresh page to trigger excessive memory usage (application will terminate)");
    response.end();
    break;
  case 1:
    console.log('heap_oom.js: allocating excessive Javascript heap memory....');
    var list = [];
    while (true) {
      list.push(new MyRecord());
    }
    response.end();
    break;
  }
}

function MyRecord() {
  this.name = 'foo';
  this.id = 128;
  this.account = 98454324;
}

var http_server = http.createServer(my_listener);
http_server.listen(8080);

console.log('fatalerror.js: Node running');
console.log('fatalerror.js: Note: heap default is 1.4Gb, use --max-old-space-size=<size in Mb> to change');
console.log('fatalerror.js: Go to http://<machine>:8080/ or http://localhost:8080/');

setTimeout(function(){
  console.log('fatalerror.js: timeout expired, exiting.');
  process.exit(0);
}, 60000);
