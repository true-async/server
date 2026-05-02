# Оптимизация HTTP-сервера — план работ

## Контекст

Per-thread бенч 2026-05-02: TAS 44.2k rps vs Swoole 46.5k rps (gap 5%).
Профилирование под `wrk -c64 -d30s`, `perf record -g -F999 --call-graph dwarf`.

Ключевые горячие точки в TAS:
- `uv_timer_start/stop` 1.55% — write deadline арм/стоп на каждый запрос
- `async_scheduler_coroutine_suspend` 0.83% — корутина уходит в await на синхронной записи
- 4 fiber switch'а на 1 синхронный запрос вместо 2
- `_emalloc/_efree` 2.21% — частично от per-request `ecalloc(ctx)` + zvals
- `zend_get_executed_filename_ex` 0.28% — лишнее на горячем пути

Главный архитектурный вывод: **запись данных делается через неправильный паттерн**.
Чтение многосшотное, без переключений; запись — через request+await+callback,
хотя ядро в 99% случаев принимает байты инлайном через `uv_try_write`.

---

## Шаг 1 — Fire-and-forget запись в hot-path HTTP/1 (текущая задача)

**Цель**: убрать suspend корутины и per-request таймер на горячем пути ответа.

### Изменения

**`php-src/Zend/zend_async_API.h`**
- Добавить поле `free_cb` к `_zend_async_io_req_s`
- Изменить typedef `zend_async_io_write_t` — 4-й параметр `free_cb`
- `ZEND_ASYNC_IO_WRITE(io, buf, count)` — макрос подставляет `NULL`
- Добавить `ZEND_ASYNC_IO_WRITE_EX(io, buf, count, free_cb)`

**`php-src/ext/async/libuv_reactor.c`**
- Сигнатура `libuv_io_write` — добавить `free_cb`
- Сохранить `free_cb` и `buf` в req
- В `io_pipe_write_cb`: если `free_cb != NULL` → вызвать `free_cb(buf, io)`,
  освободить req без NOTIFY (fire-and-forget)
- Защитная ветка в `libuv_io_req_dispose` для случая раннего close conn

**`true-async-server2/src/core/http_connection.c`**
- Убрать while-цикл в `http_connection_send_raw` (libuv делает свой партишн внутри)
- Добавить `http_connection_send_str_owned(conn, zend_string *body)` —
  fire-and-forget с передачей ownership zend_string
- В `http_handler_coroutine_dispose` (line 1567-1578) перевести hot-path записи
  на новый owned-вариант. Убрать `zend_string_release` после send

### Ожидаемый эффект
- Suspend корутины на write устраняется → 2 fiber switch'а вместо 4 на запрос
- Таймер write_timer не армится на инлайн-завершенной записи
- Сравнение с Swoole должно перевернуться в нашу пользу на синхронных handler'ах

### Проверка
- Полный phpt прогон должен проходить
- Бенч `wrk -t4 -c64 -d30s` http://localhost:8080/
- Сравнение с предыдущим baseline и Swoole
- Счётчики `[bench] fiber_switch` и `[bench] scheduler_suspend` в stderr —
  ожидаем кратное снижение

---

## Шаг 2 — TLS path на единый `_EX` API

**Контекст**: `http_connection_tls.c:535` имеет собственный fire-and-forget mechanism
(persistent send-completion callback, stash `cb->write_buf` + `cb->write_req`).
После шага 1 единый API позволяет выкинуть это в пользу `ZEND_ASYNC_IO_WRITE_EX` с `efree` free_cb.

### Изменения
- `http_connection_tls.c` — заменить ручной stash на `ZEND_ASYNC_IO_WRITE_EX(io, buf, len, efree_wrapper)`
- Удалить связанные поля из `http_connection_tls_t` (`write_req`, `write_buf`, `write_buf_len`)
- Упростить persistent callback path

### Эффект
Меньше кода, единая модель ownership, та же производительность.

---

## Шаг 3 — Per-conn write watchdog (вместо per-request timer)

**Контекст**: текущий `http_write_timer_arm/stop` пере-армирует libuv-таймер на каждый
вызов `send_raw`. После шага 1 он сработает только на партишнной записи (rare path),
но всё равно дороже одного дормантного watchdog'а на conn.

### Изменения
- В `http_connection_t` — один таймер на conn, lifecycle = lifetime of conn
- Армится один раз при первом pending-write, тикает с грубой гранулярностью (1s)
- При срабатывании пробегает `pending writes` и разрывает зависшие
- Снимает `http_write_timer_arm/stop` из `http_connection_send_raw`

### Эффект
- O(1) timer ops на conn, не на запрос
- Память — один лишний таймер на conn (16-32 байта)

---

## Шаг 4 — Vector-write (iovec) для headers + body

**Контекст**: сейчас `http_response_format` конкатенирует headers + body в один `zend_string`
через `smart_str` — лишний emalloc + memcpy. libuv нативно принимает массив `uv_buf_t[]`.

### Изменения
- Расширить API: `ZEND_ASYNC_IO_WRITEV(io, bufs, nbufs, free_cb)`
- Аналогичный `_EX` для fire-and-forget
- В `http_response_format` отдавать headers и body отдельными буферами
- В hot-path записывать через `WRITEV`

### Эффект
- Минус один alloc + memcpy на запрос (headers ~150 байт, body N байт)
- Особенно важно для streaming/large body — конкатенации избегаем целиком

---

## Шаг 5 — Coroutine context pool

**Контекст**: fiber-стек в пуле (`scheduler.c:139`, `ASYNC_FIBER_POOL_SIZE=4`),
но `async_coroutine_t` + ctx (`http1_request_ctx_t`) аллоцируются на каждый запрос.
Это часть 2.21% `_emalloc/_efree` на горячем пути.

### Изменения
- Per-thread freelist для `async_coroutine_t`
- Per-conn reuse `http1_request_ctx_t` (один ctx на conn, переинициализируется)
- HttpRequest/HttpResponse zval'ы — переиспользовать через сброс полей вместо `object_init_ex`

### Эффект
- ~1-2% CPU на горячем пути, аналогично Swoole'у

---

## Шаг 6 — `zend_get_executed_filename_ex` с горячего пути

**Контекст**: 0.28% в perf'е, вызывается из `call_user_function` для имени файла handler'а.

### Изменения
- Закешировать имя файла на `conn->handler` один раз при инициализации
- Обходить `call_user_function` через прямой вызов VM с предкешированной информацией

### Эффект
- 0.28% CPU, мелочь, бесплатно

---

## Шаг 7 — HTTP/2 hot-path

**Контекст**: HTTP/2 имеет свой dispatch path (`src/http2/http2_strategy.c:165`),
но использует те же примитивы записи. Все оптимизации шагов 1-4 применимы автоматически
после миграции на новый WRITE/WRITEV API.

### Что нужно
- Убедиться что `http2_strategy_dispatch` использует `_EX` для записи фрейма
- Профилирование h2load под `--c 64 --m 100` после применения шагов 1-4
- Возможно отдельный fast-path для маленьких DATA-фреймов

### Дополнительно
- Per-stream output buffer pooling — фреймы строятся в одном буфере и шлются батчем
- HPACK encoder state переиспользуется между запросами на одном conn (уже так)

---

## Шаг 8 — HTTP/3 hot-path

**Контекст**: QUIC поверх UDP через `ngtcp2`. Запись в UDP-сокет — `sendmsg`.
Семантика fire-and-forget там естественна.

### Что нужно
- Аналог `_EX` для UDP send (`ZEND_ASYNC_UDP_SENDTO_EX`)
  — сейчас UDP sendto уже имеет fire-and-forget паттерн через `ZEND_ASYNC_UDP_REQ_F_*` флаги,
  возможно достаточно унифицировать API
- Профилирование под h3load или curl --http3-only с keep-alive

### Дополнительно
- Generic Segmentation Offload (UDP_SEGMENT) для batch'инга мелких QUIC-пакетов
- `MSG_ZEROCOPY` для крупных DATA-frame'ов в QUIC (если они > 8 КБ)

---

## Шаг 9 — TLS оптимизация

**Контекст**: TLS добавляет overhead — encrypt в ring buffer, потом запись зашифрованного.
Шаг 2 убирает overhead на стороне записи. Сторона encrypt'а пока остаётся как есть.

### Что нужно
- Профилирование `wrk` через TLS (8443)
- Возможные улучшения:
  - kTLS (kernel TLS offload) — `setsockopt(TCP_ULP, "tls")`, шифрование ядром
  - `SSL_sendfile` для больших ответов
  - Меньше копирований между ring buffer'ами

---

## Шаг 10 — Zero-copy для больших ответов

**Контекст**: `MSG_ZEROCOPY` или `io_uring SEND_ZC` имеет смысл начиная с ~10 KB.
Для маленьких ответов вреден из-за page-pin overhead.

### Что нужно
- Threshold-based: если len > 16 КБ → fire-and-forget с zero-copy
- API: дополнительный flag в `ZEND_ASYNC_IO_WRITE_EX`
- Реализация в `libuv_reactor.c` (через прямой `send(MSG_ZEROCOPY)` минуя libuv)
  и в `iouring_reactor.c` (через `IORING_OP_SEND_ZC`)
- Уведомления через error queue (`recvmsg(MSG_ERRQUEUE)`) → free_cb

### Эффект
- 10-30% CPU экономии на крупных ответах
- На L3-кеш-bandwidth ещё ощутимее на NUMA

---

## Бенчмарк-методология

Все замеры — на `benchnet` Docker network (без NAT, без `-p`).
Базовый набор:
```
docker run -d --rm --name tas-bench --network benchnet \
    --cap-add SYS_PTRACE --cap-add SYS_ADMIN --cap-add PERFMON \
    -e WORKERS=1 tas-final \
    php -d extension=true_async_server.so /app/minimal-server.php

docker run --rm --network benchnet williamyeh/wrk \
    -t4 -c64 -d30s --latency http://tas-bench:8080/
```

Сравнение со Swoole — `swoole-perf` image, `enable_coroutine=true` (fair apples-to-apples).

Для perf:
```
docker exec tas-bench /usr/lib/linux-tools-6.8.0-111/perf record \
    -g -F 999 --call-graph dwarf -p 1 -o /tmp/perf.data -- sleep 25
```

Счётчики переключений — в `php-src/Zend/zend_fibers.c` и `php-src/ext/async/scheduler.c`,
печатают каждые 100k. Считать `count / total_requests` для switches/req.

## Базовые числа на 2026-05-02

| | rps | p50 | p99 | switches/req |
|---|---:|---:|---:|---:|
| TAS baseline | 44.2k | 1.51 ms | 1.94 ms | ~4 (estimated) |
| Swoole | 46.5k | 1.42 ms | 2.63 ms | ~2 |

Цель после шагов 1-3: **TAS ≥ Swoole по rps + сохранить p99 преимущество**.
