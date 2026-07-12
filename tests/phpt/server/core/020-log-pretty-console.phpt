--TEST--
HttpServer pretty console formatter (#5, B3): colour badge, NO_COLOR / CLICOLOR_FORCE
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_log_format_selftest')
    || !function_exists('_http_log_color_decide')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Stage B3: pretty renders "HH:MM:SS.mmm  LEVEL  body key=val …". Colour is
 * decided once at sink build and threaded through the formatter's ud; with it
 * off the output is escape-free (safe to a file). NO_COLOR disables, else
 * CLICOLOR_FORCE forces on, else colour follows isatty. */

$E = "\x1b";

/* colour OFF: exact plain golden, no escape codes. The raw "\n" in 'line' comes
 * out as \x0a — one line — so a value cannot forge a record. */
$expOff = '00:00:00.123  INFO   user login path=/a b tag=v"1 line=a\x0ab'
        . ' n=-7 sz=4294967296 ok=true r=1.5' . "\n";

$off = _http_log_format_selftest('pretty', false);
var_dump($off === $expOff);
var_dump(strpos($off, $E) === false);              // no escapes to a file

/* colour ON: dim clock, green INFO badge, dim keys, raw values. */
$expOn =
    "{$E}[2m00:00:00.123{$E}[0m  {$E}[32mINFO {$E}[0m  user login"
  . " {$E}[2mpath{$E}[0m=/a b"
  . " {$E}[2mtag{$E}[0m=v\"1"
  . " {$E}[2mline{$E}[0m=a\\x0ab"
  . " {$E}[2mn{$E}[0m=-7"
  . " {$E}[2msz{$E}[0m=4294967296"
  . " {$E}[2mok{$E}[0m=true"
  . " {$E}[2mr{$E}[0m=1.5"
  . "\n";

$on = _http_log_format_selftest('pretty', true);
var_dump($on === $expOn);

/* colour decision honours the environment (checked on a non-TTY fd). */
putenv('NO_COLOR');
putenv('CLICOLOR_FORCE');
var_dump(_http_log_color_decide());                // bare non-tty → false

putenv('NO_COLOR=1');
var_dump(_http_log_color_decide());                // NO_COLOR → false

putenv('NO_COLOR');
putenv('CLICOLOR_FORCE=1');
var_dump(_http_log_color_decide());                // CLICOLOR_FORCE → true

putenv('NO_COLOR=1');
var_dump(_http_log_color_decide());                // NO_COLOR wins over FORCE

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(false)
bool(false)
bool(true)
bool(false)
done
