--TEST--
HttpServer: H3 connection teardown cleanses sensitive struct before efree (zeroize-audit ZA-0001/0006)
--EXTENSIONS--
true_async_server
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['objdump' => true]);
?>
--FILE--
<?php
/* Regression for the ZA-0001 / ZA-0006 zeroize-audit findings: every
 * efree(http3_connection_t*) and efree(crypto_conn_ref*) in
 * src/http3/http3_connection.c must be preceded by an OPENSSL_cleanse
 * call so that the freed memory cannot be recovered by a UAF reader.
 *
 * The compiler-level proof is what we test here: the linked .o for
 * http3_connection must contain at least one OPENSSL_cleanse PLT
 * relocation for every connection-tear-down site (4 documented in
 * the audit: connection_free + crypto_conn_ref_dispose + the two
 * partial-init rollback paths). We assert an absolute floor of 4 —
 * the proxy ratio against `_efree` was abandoned because unrelated
 * code (Step 5b chunk-queue / streaming events) freely adds non-
 * sensitive efree relocs that would push the ratio under without
 * removing any cleanse. */
$candidates = [
    __DIR__ . '/../../../../src/http3/.libs/http3_connection.o',
    __DIR__ . '/../../../../src/http3/http3_connection.o',
];
$obj = null;
foreach ($candidates as $c) {
    if (is_file($c)) { $obj = $c; break; }
}
if ($obj === null) {
    echo "skip: built object not found\n";
    exit(0);
}

$out = shell_exec('objdump -d -r ' . escapeshellarg($obj) . ' 2>/dev/null');
if ($out === null || $out === '') {
    echo "fail: objdump returned nothing\n";
    exit(1);
}

$cleanse = preg_match_all('/R_X86_64_PLT(?:32|64|REL32)\s+OPENSSL_cleanse/', $out);
$efree   = preg_match_all('/R_X86_64_PLT(?:32|64|REL32)\s+_efree/', $out);

echo "OPENSSL_cleanse relocs >= 4: ", ($cleanse >= 4 ? 'yes' : "no ($cleanse)"), "\n";
echo "_efree relocs > 0: ",            ($efree > 0   ? 'yes' : "no ($efree)"),   "\n";
echo "done\n";
?>
--EXPECT--
OPENSSL_cleanse relocs >= 4: yes
_efree relocs > 0: yes
done
