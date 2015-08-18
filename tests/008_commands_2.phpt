--TEST--
Check Hiredis::appendCommand*
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->connect('localhost', 6379));
var_dump($h->appendCommand('PING'));
var_dump($h->appendCommandArray(['PING']));
var_dump($h->appendCommand('SET', 'foo2', 'bar'));
var_dump($h->appendCommandArray(['SET', 'baz2', 'quux']));
var_dump($h->appendCommand('GET', 'foo2'));
var_dump($h->appendCommandArray(['MGET', 'foo2', 'baz2']));
var_dump($h->getReply());
var_dump($h->getReply());
var_dump($h->getReply());
var_dump($h->getReply());
var_dump($h->getReply());
var_dump($h->getReply());
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
string(4) "PONG"
string(4) "PONG"
string(2) "OK"
string(2) "OK"
string(3) "bar"
array(2) {
  [0]=>
  string(3) "bar"
  [1]=>
  string(4) "quux"
}
