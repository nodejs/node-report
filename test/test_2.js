// Testcase to produce an uncaught exception
require('../').setEvents("exception");

console.log('exception.js: throwing an uncaught user exception....');

function myException(request, response) {
  throw new UserException('*** exception.js: testcase exception thrown from myException()');
}

function UserException(message) {
  this.message = message;
  this.name = "UserException";
}

myException();
