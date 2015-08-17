<?php

$h = new Hiredis();
var_dump($h->connect('', 1));
var_dump($h->setMaxReadBuf(-1));
var_dump($h->getMaxReadBuf());
var_dump($h->getLastError());
