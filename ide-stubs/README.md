# TrueAsync HTTP Server IDE Stubs

Stubs for the [TrueAsync HTTP Server](https://github.com/true-async/server)
PHP extension (`true_async_server`, PHP 8.6+).

They give editors type information for the `TrueAsync\*` classes, enums and
functions the extension registers. The file is never loaded at runtime.

Requires the [TrueAsync](https://github.com/true-async/php-async) stubs as
well — `TrueAsync\HttpException` extends `Async\AsyncCancellation`.

## PhpStorm

**Settings → PHP → Include Path** → `+` → выбрать эту папку → OK.

## Composer

```bash
composer require --dev true-async/server-stubs
```
