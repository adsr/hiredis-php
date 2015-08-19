--TEST--
Check Hiredis::setThrowExceptions
--SKIPIF--
<?php if (!extension_loaded("hiredis") || (int)shell_exec('netstat -tnlp | grep 6379 | grep redis-server | wc -l') < 1) print "skip"; ?>
--FILE--
<?php
$h = new Hiredis();
foreach ([true, false] as $on_off) {
    $h->setThrowExceptions($on_off);
    try {
        var_dump($h->set('fish', 'carp'));
        var_dump(!empty($h->getLastError()));
    } catch (Exception $e) {
        var_dump(get_class($e));
    }
}
--EXPECT--
string(16) "HiredisException"
bool(false)
bool(true)
