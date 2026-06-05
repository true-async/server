--TEST--
StaticHandler: argument validation + setter/getter coverage (no server)
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Pure offline coverage of StaticHandler's PHP surface: constructor
 * validation branches + every setter's argument-error path + getters.
 * No HttpServer instance is created — keeps the test cheap and focused
 * on the C class's parameter-parsing/validation arms. */

use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use TrueAsync\StaticDotfiles;
use TrueAsync\StaticSymlinks;

/* Build a usable docroot for the "happy path" cases. */
$root = sys_get_temp_dir() . '/sh-val-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/marker", "x");
register_shutdown_function(function() use ($root) {
    @unlink("$root/marker");
    @rmdir($root);
});

/* Helper — execute $fn and report the exception class + message head,
 * or "OK" if it returned normally. Lets the EXPECT block be a flat
 * line-per-case list. */
function check(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: OK\n";
    } catch (\Throwable $e) {
        echo "$label: ", $e::class, ": ", $e->getMessage(), "\n";
    }
}

/* ---- Constructor: url-prefix validation ------------------------ */
check('ctor:prefix-empty',       fn() => new StaticHandler('',         $root));
check('ctor:prefix-no-lead',     fn() => new StaticHandler('static/',  $root));
check('ctor:prefix-no-trail',    fn() => new StaticHandler('/static',  $root));
check('ctor:prefix-nul',         fn() => new StaticHandler("/a\0/",    $root));
check('ctor:prefix-double-slash', fn() => new StaticHandler('/a//b/',  $root));
check('ctor:prefix-too-short',   fn() => new StaticHandler('/',        $root)); // len < 2

/* ---- Constructor: root-directory validation -------------------- */
/* Cross-platform: feed an absolute-but-missing path under the system
 * temp dir so it clears the absolute-path gate and hits the not-found
 * branch on every OS (a bare "/..." path is NOT absolute on Windows).
 * The POSIX-only root cases ("/" itself) live in 016-...-posix. */
$absent_root = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'sh-absent-' . bin2hex(random_bytes(8));
check('ctor:root-empty',         fn() => new StaticHandler('/x/', ''));
check('ctor:root-relative',      fn() => new StaticHandler('/x/', 'relative'));
check('ctor:root-missing',       fn() => new StaticHandler('/x/', $absent_root));
check('ctor:root-not-a-dir',     fn() => new StaticHandler('/x/', __FILE__));

/* ---- Happy path ------------------------------------------------ */
$sh = new StaticHandler('/static/', $root);
$gr = $sh->getRootDirectory();
/* "absolute for this OS": leading slash on *nix, drive-letter / UNC on Windows. */
$root_abs = $gr !== '' && (str_starts_with($gr, '/') || str_starts_with($gr, '\\')
    || (strlen($gr) >= 2 && ctype_alpha($gr[0]) && $gr[1] === ':'));
echo "happy-path: prefix=", $sh->getUrlPrefix(), " root-ok=", $root_abs ? 'yes' : 'no', "\n";
echo "isLocked: ", $sh->isLocked() ? 'yes' : 'no', "\n";

/* ---- setIndexFiles: validation arms --------------------------- */
check('idx:non-string',  fn() => $sh->setIndexFiles('ok.html', 123));
check('idx:empty',       fn() => $sh->setIndexFiles(''));
check('idx:slash',       fn() => $sh->setIndexFiles('sub/index.html'));
check('idx:happy',       fn() => $sh->setIndexFiles('index.html', 'home.html'));
check('idx:zero-args',   fn() => $sh->setIndexFiles());                  // clears list

/* ---- disableIndex / setOnMissing ------------------------------ */
check('disableIndex',         fn() => $sh->disableIndex());
check('setOnMissing:NEXT',    fn() => $sh->setOnMissing(StaticOnMissing::NEXT));
check('setOnMissing:NOT_FND', fn() => $sh->setOnMissing(StaticOnMissing::NOT_FOUND));

/* ---- enablePrecompressed -------------------------------------- */
check('precomp:non-string',  fn() => $sh->enablePrecompressed('br', 1));
check('precomp:unknown',     fn() => $sh->enablePrecompressed('snappy'));
check('precomp:all-known',   fn() => $sh->enablePrecompressed('br', 'gzip', 'zstd'));
check('precomp:disable',     fn() => $sh->disablePrecompressed());

/* ---- setDotfilePolicy / setSymlinkPolicy ---------------------- */
foreach ([StaticDotfiles::DENY, StaticDotfiles::ALLOW, StaticDotfiles::IGNORE] as $p) {
    check('dotfile:' . $p->name, fn() => $sh->setDotfilePolicy($p));
}
foreach ([StaticSymlinks::REJECT, StaticSymlinks::FOLLOW, StaticSymlinks::OWNER_MATCH] as $p) {
    check('symlink:' . $p->name, fn() => $sh->setSymlinkPolicy($p));
}

/* ---- hide ------------------------------------------------------ */
check('hide:non-string',   fn() => $sh->hide('*.bak', 42));
check('hide:empty',        fn() => $sh->hide(''));
check('hide:happy',        fn() => $sh->hide('*.bak', '.git/*'));
check('hide:append',       fn() => $sh->hide('*.tmp'));   // exercises erealloc branch

/* ---- setEtagEnabled / setBrowseEnabled ------------------------ */
check('etag-on',    fn() => $sh->setEtagEnabled(true));
check('etag-off',   fn() => $sh->setEtagEnabled(false));
check('browse-on',  fn() => $sh->setBrowseEnabled(true));
check('browse-off', fn() => $sh->setBrowseEnabled(false));

/* ---- setOpenFileCache / disableOpenFileCache ------------------ */
check('ofc:negative',    fn() => $sh->setOpenFileCache(-1));
check('ofc:ttl-negative', fn() => $sh->setOpenFileCache(100, -1));
check('ofc:happy',       fn() => $sh->setOpenFileCache(256, 30));
check('ofc:disable',     fn() => $sh->disableOpenFileCache());

/* ---- setCacheControl ------------------------------------------ */
check('cc:crlf',    fn() => $sh->setCacheControl("public\r\ninjected"));
check('cc:empty',   fn() => $sh->setCacheControl(''));         // clears
check('cc:happy',   fn() => $sh->setCacheControl('public, max-age=3600'));
check('cc:rewrite', fn() => $sh->setCacheControl('no-cache'));  // exercises release-old branch

/* ---- setHeader ------------------------------------------------- */
check('hdr:empty-name',   fn() => $sh->setHeader('', 'v'));
check('hdr:ctl-name',     fn() => $sh->setHeader("X-\tBad", 'v'));
check('hdr:colon-name',   fn() => $sh->setHeader('X:Y', 'v'));
check('hdr:ctl-value',    fn() => $sh->setHeader('X-Ok', "a\r\nb"));
check('hdr:nul-value',    fn() => $sh->setHeader('X-Ok', "a\0b"));
check('hdr:happy',        fn() => $sh->setHeader('X-Powered-By', 'true-async'));
check('hdr:overwrite',    fn() => $sh->setHeader('X-Powered-By', 'true-async-2'));

/* ---- setMimeType ---------------------------------------------- */
check('mime:empty-ext',   fn() => $sh->setMimeType('', 'text/plain'));
check('mime:dot-ext',     fn() => $sh->setMimeType('.txt', 'text/plain'));
check('mime:empty-ct',    fn() => $sh->setMimeType('foo', ''));
check('mime:ctl-ct',      fn() => $sh->setMimeType('foo', "text/plain\r\nX-Inject: y"));
check('mime:happy',       fn() => $sh->setMimeType('mdx', 'text/markdown'));
check('mime:overwrite',   fn() => $sh->setMimeType('mdx', 'text/markdown; charset=utf-8'));

echo "done\n";
?>
--EXPECTF--
ctor:prefix-empty: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must start and end with '/'
ctor:prefix-no-lead: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must start and end with '/'
ctor:prefix-no-trail: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must start and end with '/'
ctor:prefix-nul: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must not contain NUL
ctor:prefix-double-slash: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must not contain '//'
ctor:prefix-too-short: TrueAsync\HttpServerInvalidArgumentException: StaticHandler url prefix must start and end with '/'
ctor:root-empty: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory must not be empty
ctor:root-relative: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory must be an absolute path
ctor:root-missing: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory not found: %s
ctor:root-not-a-dir: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory is not a directory: %s
happy-path: prefix=/static/ root-ok=yes
isLocked: no
idx:non-string: TrueAsync\HttpServerInvalidArgumentException: StaticHandler index files must be strings
idx:empty: TrueAsync\HttpServerInvalidArgumentException: StaticHandler index file name must not be empty
idx:slash: TrueAsync\HttpServerInvalidArgumentException: StaticHandler index file name must not contain '/': sub/index.html
idx:happy: OK
idx:zero-args: %s
disableIndex: OK
setOnMissing:NEXT: OK
setOnMissing:NOT_FND: OK
precomp:non-string: TrueAsync\HttpServerInvalidArgumentException: StaticHandler precompressed encoding name must be a string
precomp:unknown: TrueAsync\HttpServerInvalidArgumentException: StaticHandler unknown precompressed encoding 'snappy' (expected one of: br, gzip, zstd)
precomp:all-known: OK
precomp:disable: OK
dotfile:DENY: OK
dotfile:ALLOW: OK
dotfile:IGNORE: OK
symlink:REJECT: OK
symlink:FOLLOW: OK
symlink:OWNER_MATCH: OK
hide:non-string: TrueAsync\HttpServerInvalidArgumentException: StaticHandler hide pattern must be a string
hide:empty: TrueAsync\HttpServerInvalidArgumentException: StaticHandler hide pattern must not be empty
hide:happy: OK
hide:append: OK
etag-on: OK
etag-off: OK
browse-on: OK
browse-off: OK
ofc:negative: TrueAsync\HttpServerInvalidArgumentException: StaticHandler::setOpenFileCache(): maxEntries must be between 0 and INT32_MAX
ofc:ttl-negative: TrueAsync\HttpServerInvalidArgumentException: StaticHandler::setOpenFileCache(): ttlSeconds must be between 0 and INT32_MAX
ofc:happy: OK
ofc:disable: OK
cc:crlf: TrueAsync\HttpServerInvalidArgumentException: StaticHandler cache-control value contains a control character
cc:empty: OK
cc:happy: OK
cc:rewrite: OK
hdr:empty-name: TrueAsync\HttpServerInvalidArgumentException: StaticHandler header name must not be empty
hdr:ctl-name: TrueAsync\HttpServerInvalidArgumentException: StaticHandler header name contains an invalid character
hdr:colon-name: TrueAsync\HttpServerInvalidArgumentException: StaticHandler header name contains an invalid character
hdr:ctl-value: TrueAsync\HttpServerInvalidArgumentException: StaticHandler header value contains a control character
hdr:nul-value: TrueAsync\HttpServerInvalidArgumentException: StaticHandler header value contains a control character
hdr:happy: OK
hdr:overwrite: OK
mime:empty-ext: TrueAsync\HttpServerInvalidArgumentException: StaticHandler MIME extension must not be empty
mime:dot-ext: TrueAsync\HttpServerInvalidArgumentException: StaticHandler MIME extension must not include the leading '.'
mime:empty-ct: TrueAsync\HttpServerInvalidArgumentException: StaticHandler content type must not be empty
mime:ctl-ct: TrueAsync\HttpServerInvalidArgumentException: StaticHandler content type contains a control character
mime:happy: OK
mime:overwrite: OK
done
