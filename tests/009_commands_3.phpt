--TEST--
Check Hiredis::(client|set|strlen)*
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
var_dump($h->connect('localhost', 6379));
var_dump($h->client('SETNAME', 'Bob'));
var_dump($h->client('GETNAME'));
var_dump($h->set('quux2', "binary\x00safe"));
var_dump($h->strlen('quux2'));
--EXPECT--
bool(true)
string(2) "OK"
string(3) "Bob"
string(2) "OK"
int(11)
