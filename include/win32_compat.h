/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Windows / MSVC portability shims — only what php.h does not already cover.

  php.h (and the headers it pulls in) already provides on Windows:
    ssize_t        — main/config.w32.h: #define ssize_t SSIZE_T
    S_ISREG/DIR/LNK— Zend/zend_virtual_cwd.h
    MAXPATHLEN     — win32/ioutil.h (2048, not the 260 of MAX_PATH)
    lstat()        — php.h → php_sys_lstat → php_win32_ioutil_lstat
    realpath()     — win32/ioutil.h (proper UTF-8 implementation)
    fstat()/stat() — ucrt <sys/stat.h> static inlines
    O_RDONLY/WRONLY/RDWR/CREAT/TRUNC — ucrt <fcntl.h>

  This header adds only the remaining gaps:
    O_CLOEXEC, O_NOFOLLOW   — Linux/BSD extensions absent from ucrt
    open()                  — no bare alias in ucrt; map to PHP's UTF-8 wrapper
    close/read/unlink       — MSVC deprecates the bare POSIX names

  Include order: MUST come after <php.h> so PHP_WIN32 is defined and
  win32/ioutil.h (which declares php_win32_ioutil_open) is already visible.
*/

#ifndef HTTP_SERVER_WIN32_COMPAT_H
#define HTTP_SERVER_WIN32_COMPAT_H

#ifdef PHP_WIN32

/* O_CLOEXEC: HANDLE inheritance is opt-in on Windows (close-on-exec is the
 * default), so mapping to 0 is correct.
 * O_NOFOLLOW: enforced in software by http_static_symlink_policy_admits(). */
# ifndef O_CLOEXEC
#  define O_CLOEXEC  0
# endif
# ifndef O_NOFOLLOW
#  define O_NOFOLLOW 0
# endif

/* ucrt has no bare open(); use PHP's UTF-8-aware wrapper (declared in
 * win32/ioutil.h, pulled in by php.h → zend_virtual_cwd.h). */
# ifndef open
#  define open(path, flags, ...)  php_win32_ioutil_open((path), (flags), ##__VA_ARGS__)
# endif

/* POSIX names deprecated in MSVC: map to the _-prefixed CRT equivalents.
 * close(fd) here is always a CRT file-descriptor op, never closesocket().
 * read() needs an unsigned-int cast because _read() requires it.
 * unlink() here is CRT only; PHP's VFS layer uses VCWD_UNLINK(). */
# include <io.h>
# ifndef close
#  define close(fd)           _close(fd)
# endif
# ifndef read
#  define read(fd, buf, n)    _read((fd), (buf), (unsigned int)(n))
# endif
# ifndef unlink
#  define unlink(path)        _unlink(path)
# endif

#endif /* PHP_WIN32 */

#endif /* HTTP_SERVER_WIN32_COMPAT_H */
