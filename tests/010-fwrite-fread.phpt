--TEST--
Calls to fwrite and fread are logged
--SKIPIF--
<?php if (!extension_loaded("scoutapm")) die("Skipped: scoutapm extension required."); ?>
--FILE--
<?php
$fh = tmpfile();

fwrite($fh, "fread/fwrite test\n");
fseek($fh, 0);
echo fread($fh, 18);
fclose($fh);

$calls = scoutapm_get_calls();

var_dump($calls[0]['function']);
var_dump($calls[0]['argv']);

var_dump($calls[1]['function']);
var_dump($calls[1]['argv']);
?>
--EXPECTF--
fread/fwrite test
string(6) "fwrite"
array(2) {
  [0]=>
  resource(%d) of type (Unknown)
  [1]=>
  string(18) "fread/fwrite test
"
}
string(5) "fread"
array(2) {
  [0]=>
  resource(%d) of type (Unknown)
  [1]=>
  int(18)
}
