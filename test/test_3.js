// Testcase to produce a fatal error (javascript heap OOM)
require('../').setEvents("fatalerror");

console.log('fatalerror.js: allocating excessive javascript heap memory....');
var list = [];
while (true) {
  var record = new MyRecord();
  list.push(record);
}


function MyRecord() {
  this.name = 'foo';
  this.id = 128;
  this.account = 98454324;
}
