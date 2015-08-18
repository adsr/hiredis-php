--TEST--
Check Hiredis::(set|get)*
--SKIPIF--
<?php if (!extension_loaded("hiredis")) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
$h->setTimeout(1);
$h->setKeepAliveInterval(2);
$h->setMaxReadBuf(3);
var_dump($h->getTimeout());
var_dump($h->getKeepAliveInterval());
var_dump($h->getMaxReadBuf());
--EXPECT--
int(1)
int(2)
int(3)
