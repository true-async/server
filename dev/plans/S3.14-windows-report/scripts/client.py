"""One burst client: opens N non-blocking connects at once, sends one
HTTP/1.0 request on each and reads it to EOF.

The process arms itself (writes <out>.armed), spins until the go file exists,
then starts. Times are time.perf_counter_ns(), which on Windows is the
system-wide QueryPerformanceCounter, so values from different processes share
one clock (checked by clockcheck.py).

Usage: client.py <port> <n> <go file> <out json>
Output: first_connect_ns, last_done_ns, statuses {first status line: count},
errors [{kind, detail}], unfinished count.
"""
import errno
import json
import os
import selectors
import socket
import sys
import time

REQUEST = b"GET / HTTP/1.0\r\nHost: x\r\n\r\n"
DEADLINE_S = 30.0


def main():
    port, n, go_file, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    with open(out + ".armed", "w"):
        pass
    while not os.path.exists(go_file):
        pass

    sel = selectors.DefaultSelector()
    bufs = {}
    errors = []
    statuses = {}

    first = time.perf_counter_ns()
    for _ in range(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        rc = s.connect_ex(("127.0.0.1", port))
        if rc not in (0, errno.EWOULDBLOCK, errno.EINPROGRESS, 10035):
            errors.append({"kind": "connect", "detail": os.strerror(rc) if rc < 10000 else str(rc)})
            s.close()
            continue
        bufs[s] = None
        sel.register(s, selectors.EVENT_WRITE)

    last = first
    deadline = time.monotonic() + DEADLINE_S
    while bufs and time.monotonic() < deadline:
        for key, ev in sel.select(timeout=1.0):
            s = key.fileobj
            if bufs[s] is None:
                # Writable: the connect finished, one way or the other.
                err = s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if err:
                    errors.append({"kind": "connect", "detail": str(err)})
                    sel.unregister(s)
                    s.close()
                    del bufs[s]
                    continue
                s.sendall(REQUEST)
                bufs[s] = bytearray()
                sel.modify(s, selectors.EVENT_READ)
                continue
            try:
                chunk = s.recv(65536)
            except OSError as e:
                errors.append({"kind": "recv", "detail": str(e)})
                chunk = b""
                bufs[s] = None
            if chunk:
                bufs[s] += chunk
                continue
            last = time.perf_counter_ns()
            data = bufs[s]
            if data is not None:
                line = bytes(data.split(b"\r\n", 1)[0]).decode("latin-1") or "<empty>"
                statuses[line] = statuses.get(line, 0) + 1
            sel.unregister(s)
            s.close()
            del bufs[s]

    unfinished = len(bufs)
    for s in list(bufs):
        s.close()

    with open(out, "w") as f:
        json.dump({"first_connect_ns": first, "last_done_ns": last, "statuses": statuses,
                   "errors": errors, "unfinished": unfinished}, f)


if __name__ == "__main__":
    main()
