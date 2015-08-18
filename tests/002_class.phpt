--TEST--
Check for Hiredis class
--SKIPIF--
<?php if (!extension_loaded("hiredis")) print "skip"; ?>
--FILE--
<?php
var_dump(class_exists('Hiredis'));
--EXPECT--
bool(true)
