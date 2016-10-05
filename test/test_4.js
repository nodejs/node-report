// Testcase to loop in Javascript code
require('nodereport');

console.log('loop.js: going into loop now.... use kill -USR2 <pid> to trigger NodeReport');

function myLoop() {
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
}

function MyRecord() {
  this.name = 'foo';
  this.id = 128;
  this.account = 98454324;
}

myLoop();
