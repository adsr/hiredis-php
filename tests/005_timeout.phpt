--TEST--
Check Hiredis::setTimeout
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
$h->setTimeout(1);
var_dump($h->connect('localhost', 6379));
var_dump($h->getReply());
var_dump(!empty($h->getLastError()));
--EXPECT--
bool(true)
bool(false)
bool(true)
