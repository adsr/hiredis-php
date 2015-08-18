--TEST--
Check Hiredis::getLastError
--SKIPIF--
<?php if (!extension_loaded("hiredis")) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->command('derp'));
var_dump($h->getLastError());
--EXPECT--
bool(false)
string(15) "No redisContext"
