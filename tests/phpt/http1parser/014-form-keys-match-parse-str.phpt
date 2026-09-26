--TEST--
HTTP/1 parser: getQuery() and an url-encoded getPost() key their arrays exactly as parse_str() does
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* parse_str() runs PHP's own php_register_variable_ex, so it is the oracle for
 * the name rules the server reimplements. */

$corpus = [
    'a=1&b=2',
    'list[]=1&list[]=2&list[]=3',
    'map[key]=v&map[other]=w',
    'm[0][1]=z&m[0][2]=y',
    'a.b=1&c d=2&e+f=3',
    '+++lead=1&%20%20x=2',
    'a[b=1',
    'a[b][c=1',
    'a[b]c=1',
    'a[b]c[d]=1',
    'a[ ]=1&a[ ]=2&a[  ]=3',
    'a[.b]=1&a[ b]=2',
    '123=num&a[7]=seven&a[07]=zero-seven&a[-3]=neg',
    'a=1&a[]=2',
    'a[]=1&a=2',
    '[x]=dropped&=empty&ok=1',
    'noval&also=&=&&&x=1',
    'enc%5B%5D=1&enc%5Bk%5D=2&%61=3',
    'v=%00bin%FF+plus%2B&w=%zz&x=%4',
    'n%00ame=1&o[k%00ey]=2',
    'a[b][c][d][e]=deep',
    'GLOBALS=1&this=2&__Host-x=3',
    'a[__Host-x]=1&a[__Secure-y][z]=2&__Secure-w=4&b[x][__Host-q]=5',
];

$query = static fn(string $qs): array => TrueAsync\http_parse_request("GET /?$qs HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery();

$post = static fn(string $body): array => TrueAsync\http_parse_request(
    "POST / HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body
)->getPost();

foreach ($corpus as $input) {
    $expected = [];
    parse_str($input, $expected);

    $got_query = $query(str_replace(' ', '%20', $input));
    $got_post  = $post($input);

    printf("%-40s query=%s post=%s\n", $input,
        $got_query === $expected ? 'same' : 'DIFF ' . json_encode($got_query) . ' vs ' . json_encode($expected),
        $got_post === $expected ? 'same' : 'DIFF ' . json_encode($got_post) . ' vs ' . json_encode($expected));
}
?>
--EXPECT--
a=1&b=2                                  query=same post=same
list[]=1&list[]=2&list[]=3               query=same post=same
map[key]=v&map[other]=w                  query=same post=same
m[0][1]=z&m[0][2]=y                      query=same post=same
a.b=1&c d=2&e+f=3                        query=same post=same
+++lead=1&%20%20x=2                      query=same post=same
a[b=1                                    query=same post=same
a[b][c=1                                 query=same post=same
a[b]c=1                                  query=same post=same
a[b]c[d]=1                               query=same post=same
a[ ]=1&a[ ]=2&a[  ]=3                    query=same post=same
a[.b]=1&a[ b]=2                          query=same post=same
123=num&a[7]=seven&a[07]=zero-seven&a[-3]=neg query=same post=same
a=1&a[]=2                                query=same post=same
a[]=1&a=2                                query=same post=same
[x]=dropped&=empty&ok=1                  query=same post=same
noval&also=&=&&&x=1                      query=same post=same
enc%5B%5D=1&enc%5Bk%5D=2&%61=3           query=same post=same
v=%00bin%FF+plus%2B&w=%zz&x=%4           query=same post=same
n%00ame=1&o[k%00ey]=2                    query=same post=same
a[b][c][d][e]=deep                       query=same post=same
GLOBALS=1&this=2&__Host-x=3              query=same post=same
a[__Host-x]=1&a[__Secure-y][z]=2&__Secure-w=4&b[x][__Host-q]=5 query=same post=same
