--TEST--
HttpResponse: Private constructor
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpResponse;

// HttpResponse cannot be instantiated directly
try {
    $response = new HttpResponse();
    echo "FAIL: Should have thrown an error\n";
} catch (Error $e) {
    echo "OK: " . get_class($e) . "\n";
    echo "Message contains 'private': " . (str_contains($e->getMessage(), 'private') ? 'yes' : 'no') . "\n";
}

echo "Done\n";
--EXPECT--
OK: Error
Message contains 'private': yes
Done
