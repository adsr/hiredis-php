--TEST--
Check Hiredis::z*
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->connect('localhost', 6379));
$h->zrem('quuxset', 'one', 'two', 'three');
var_dump($h->zadd('quuxset', 1, 'one'));
var_dump($h->zadd('quuxset', 2, 'two'));
var_dump($h->zadd('quuxset', 3, 'three'));
var_dump($h->zrevrange('quuxset', 0, -1));
--EXPECT--
bool(true)
int(1)
int(1)
int(1)
array(3) {
  [0]=>
  string(5) "three"
  [1]=>
  string(3) "two"
  [2]=>
  string(3) "one"
}
