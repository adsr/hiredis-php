--TEST--
Check Hiredis::sendRaw*
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->connect('localhost', 6379));
var_dump($h->sendRaw('PING'));
var_dump($h->sendRawArray(['PING']));
var_dump($h->sendRaw('SET', 'foo', 'bar'));
var_dump($h->sendRawArray(['SET', 'baz', 'quux']));
var_dump($h->sendRaw('GET', 'foo'));
var_dump($h->sendRawArray(['MGET', 'foo', 'baz']));
--EXPECT--
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
