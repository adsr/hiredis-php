--TEST--
Check Hiredis::connect
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->connect('localhost', 6379));
--EXPECT--
bool(true)
