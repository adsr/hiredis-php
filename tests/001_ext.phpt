--TEST--
Check for hiredis presence
--SKIPIF--
<?php if (!extension_loaded("hiredis")) print "skip"; ?>
--FILE--
<?php
echo "hiredis extension is available";
--EXPECT--
hiredis extension is available
