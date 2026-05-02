# Оптимизация HTTP-сервера — план работ

> **Статус на 2026-05-02**: шаги 1, 3.0a, 3.1, 3.3, добавление
> `zend_async_now()` API, coalesce `zend_hrtime`, шаги 4.1, 10, 6 —
> **выполнены**, в `main`/`feat/multishot-alloc-cb`. Подробный список —
> в разделе «Что уже сделано» в конце документа.

> **TLS — починен 2026-05-02 (uncommitted)**. Регресс был вызван
> `http_connection_alloc_cb` (введён в шаге 3.1 вместе с slab-arena):
> в `libuv_io_alloc_cb` он имеет приоритет над per-req буфером, и
> ciphertext из libuv писался в plaintext `read_buffer` вместо
> зарезервированного слота BIO-кольца, тогда как BIO write head
> продвигался по `tls_commit_cipher_in` — `SSL_do_handshake` читал
> мусор и алёртил decode_error. Фикс — обнулить `conn->io->alloc_cb`
> для TLS connections. Все 14 TLS phpt + оба H2-over-TLS зелёные.

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
- При желании — ad-hoc счётчики `[bench] fiber_switch` / `[bench] scheduler_suspend`
  в stderr (инструментация локальная, не коммитится); ожидаем кратное снижение

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

## Шаг 3 — Один global periodic watchdog + conn arena с alive-list

**Контекст**: текущая модель таймаутов фрагментирована —
- `http_write_timer` per-conn lazy timer, arm/stop per send (теперь не на горячем
  пути после шага 1, но всё равно работает в TLS / partial-write случаях),
- per-await read-timeout (`async_io_req_await:557-563` создаёт **новый**
  `ZEND_ASYNC_NEW_TIMER_EVENT` на КАЖДЫЙ read await),
- keepalive_timeout логически отдельная семантика.

Каждое из этих мест — это операции с heap'ом libuv.

### Архитектура

**Один periodic watchdog на worker thread** + **slab arena conn'ов с alive-list'ом**.

#### Conn arena (slab + intrusive lists)

```c
typedef struct conn_chunk_s {
    struct conn_chunk_s *next_chunk;
    http_connection_t    slots[CONNS_PER_CHUNK];   // 256
} conn_chunk_t;

typedef struct conn_arena_s {
    conn_chunk_t      *chunks;       // chain of chunks
    http_connection_t *free_head;    // single-linked freelist (через `next`)
    http_connection_t *alive_head;   // double-linked alive-list (через `next`/`prev`)
    size_t             live_count;
} conn_arena_t;
```

В `http_connection_t` добавляется:
```c
http_connection_t *next, *prev;   // в alive: двусвязный; в free: только `next` (single-linked)
uint64_t           deadline_ms;   // 0 = нет активного дедлайна
```

Поля `next`/`prev` играют две роли в зависимости от состояния слота. По
аналогии с Zend MM small-bins.

#### Alloc/free

- `conn_arena_alloc()` — pop из free_head; если пусто, аллоцируется новый чанк
  и его 256 слотов добавляются в free-list. Затем conn пушится в alive-list.
- `conn_arena_free()` — remove из alive-list, push в free-list. Никаких
  `pemalloc/pefree` на горячем пути.

#### Watchdog

Один periodic `deadline_tick` per-worker:
```c
uint32_t tick_ms = MIN(read_timeout_ms, write_timeout_ms, keepalive_timeout_ms) / 2;
if (tick_ms < 250) tick_ms = 250;        // floor для коротких таймаутов

server->deadline_tick = ZEND_ASYNC_NEW_TIMER_EVENT(tick_ms, /*periodic*/ true);
ZEND_ASYNC_TIMER_SET_MULTISHOT(server->deadline_tick);
server->deadline_tick->add_callback(deadline_tick, deadline_tick_cb);
server->deadline_tick->start(deadline_tick);
```

При типичных дефолтах (15s/15s/60s): tick = **7.5s**. Окно убийства conn'а
с истёкшим дедлайном = `[deadline, deadline + 7.5s]`. Для HTTP это адекватно.

В колбэке tick'а:
```c
static void deadline_tick_cb(...) {
    uint64_t now = now_ms();
    http_connection_t *c, *next;
    for (c = arena->alive_head; c; c = next) {
        next = c->next;     // safe — c может быть удалён внутри
        if (c->deadline_ms != 0 && now >= c->deadline_ms) {
            force_close(c);  // marks for destroy, removes from alive-list
        }
    }
}
```

#### Установка дедлайнов в горячем пути

На точках смены состояния — **просто store**:

```c
// На старте чтения headers:
conn->deadline_ms = now_ms() + read_timeout_ms;

// На старте записи (если IO_WRITE_EX вернул не completed=true):
conn->deadline_ms = now_ms() + write_timeout_ms;

// При переходе в keepalive:
conn->deadline_ms = now_ms() + keepalive_timeout_ms;

// При завершении операции (опционально):
conn->deadline_ms = 0;       // явно "нет дедлайна"
```

Никаких `uv_timer_start`, `uv_timer_stop`, никаких heap-операций.

### Что удаляем

- `http_write_timer`, `http_write_timer_arm`, `http_write_timer_stop`,
  `http_write_timer_cb_fn`, `http_write_timer_cb_dispose`,
  `http_write_timer_dispose` — целиком.
- Поля `write_timer`, `write_timer_cb` из `http_connection_t`.
- Read-timeout таймер в `async_io_req_await:557-563` (или сделать его no-op
  для callers, мигрировавших на deadline_ms-модель).

### Бонус — broadcast / admission / shutdown

`alive_head` пригоден для всего, что требует обхода живых conn'ов:
WebSocket broadcast, admission control sweep под нагрузкой, graceful shutdown,
`/server/status`. Не нужно строить отдельные структуры под каждое.

### Эффект
- На горячем пути — **0 timer-ops, 0 heap-ops** на запрос. Только 1 store.
- На worker thread — **1 timer** в libuv-heap независимо от числа conn'ов.
- Аллокация conn'а — без `pemalloc` (pool'ed).
- Сканирование alive-list cache-friendly за счёт slab'а.

### Порядок реализации

1. Ввести `conn_arena_t` per-worker (где живёт глобальное состояние server thread'а).
2. Заменить `http_connection_create`/`destroy` на arena alloc/free + init.
3. Добавить `next/prev/deadline_ms` в `http_connection_t`.
4. На старте worker'а — создать `deadline_tick` с динамической гранулярностью.
5. В колбэке tick'а — обход alive-list и `force_close` expired'ов.
6. На точках смены состояния — `set_deadline(conn, ms)` (1 store).
7. Удалить `http_write_timer*` и связанный код.
8. Удалить read-timeout таймер из `async_io_req_await`.

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

Счётчики переключений (`[bench] fiber_switch` / `[bench] scheduler_suspend`) — ad-hoc
инструментация в `php-src/Zend/zend_fibers.c` и `php-src/ext/async/scheduler.c`,
вшивается локально на время замеров и убирается перед коммитом.

## Базовые числа на 2026-05-02

| | rps | p50 | p99 | switches/req |
|---|---:|---:|---:|---:|
| TAS baseline | 44.2k | 1.51 ms | 1.94 ms | ~4 (estimated) |
| Swoole | 46.5k | 1.42 ms | 2.63 ms | ~2 |

Цель после шагов 1-3: **TAS ≥ Swoole по rps + сохранить p99 преимущество**.

---

## Что уже сделано (2026-05-02)

| шаг | коммит | что |
|---|---|---|
| 1 | `c5fd2c9` | Fire-and-forget plaintext write (`http_connection_send_str_owned`), убран мёртвый while-цикл в `send_raw`, новый API `ZEND_ASYNC_IO_WRITE_EX` с `free_cb` (php-src `124622ca59` + ext/async `24bb0c1`). |
| 3.0a | `dc548c0` | Split `http_server_object`: PHP-обёртка `http_server_php { server*; std }`, refcount на C-state. Conn'ы держат ref, C-state переживает PHP wrapper. |
| 3.1 | `29fd57e` | Slab arena для `http_connection_t`: 256 слотов на чанк, embedded freelist (`next_conn`), doubly-linked alive list (`next_conn`/`prev_conn`). |
| 3.3 | `4a14ec8` | Periodic `deadline_tick` watchdog на worker thread. Tick = `max(250, min(read,write,keepalive)/2)`. Заменил per-conn write_timer и per-await read-timeout. Keepalive timeout наконец-то enforced. |
| extra | `b9d74fe974` (php-src) + `45e599a` (ext/async) | API `zend_async_now()` — cached loop time в ms, через `uv_now()`. |
| extra | `14f3b1f` | `deadline_ns` → `deadline_ms` через `ZEND_ASYNC_NOW()`. |
| extra | `7015c9f` | Coalesce `zend_hrtime`: `on_request_sample` и `should_drain_now`/`drain_evaluate` теперь принимают `now_ns` параметром, переиспользуют `req->end_ns`. Минус 2 zend_hrtime per request. |
| docs | `ffee257` | `docs/PERF_2026_05_02_STEPS_1_3.md` — историческая запись результатов. |

### Перфо-итоги

| | rps c=64 | p99 | vdso_clock_gettime |
|---|---:|---:|---:|
| baseline (вчера) | 44.2k | 1.94 ms | 1.46% (5 zend_hrtime + libuv) |
| step 1 | 65.6k | 1.78 ms | 1.46% |
| step 3.3 | 66.3k | 1.38 ms | 1.46% |
| step 4 (coalesce) | 60-66k шум | 1.38 ms | **0.74%** |

Bench-номер на step 4 шумит из-за WSL2/docker network — медиана в районе 60-65k. p99 стабилен. Реальное улучшение — на CPU-budget (vdso в 2 раза меньше → ~5 ms/sec saved).

---

## Что дальше

### ~~Шаг 0 — TLS handshake regression~~ ✓ (uncommitted)

**Причина**: `http_connection_alloc_cb` (multishot per-chunk allocator,
введён вместе с шагом 3.1) был зарегистрирован на `conn->io`
безусловно — и `libuv_io_alloc_cb` всегда отдаёт ему приоритет, даже
для one-shot read'ов с явно переданным буфером. В TLS-пути
`tls_arm_one_shot_read` резервирует слот в BIO-кольце через
`BIO_nwrite0` и передаёт его в `ZEND_ASYNC_IO_READ(io, slot, space)`,
но libuv писал ciphertext в plaintext `read_buffer` (то, что
возвращает alloc_cb), а `tls_commit_cipher_in(n)` затем продвигал
write head BIO как будто байты лежат в кольце. `SSL_do_handshake`
читал нулевой мусор из BIO и возвращал decode_error → сервер закрывал
соединение, клиент видел `unexpected eof`.

**Фикс**: после `ZEND_ASYNC_IO_CLR_MULTISHOT(conn->io)` для TLS
обнуляем `conn->io->alloc_cb = NULL`. Per-req путь (`req->base.buf`)
теперь действительно используется.

**Проверка**: 14/15 TLS phpt PASS (один `SKIP` — нет `ext/openssl` в
CLI-сборке для `ssl://` транспорта в тесте), оба H2-over-TLS PASS.
Преды­дущие 10 TLS phpt + 2 H2-over-TLS regression закрыты.
Plaintext H1/H2 регрессов нет (два падения в `h1/005` и `h2/009-h2spec`
existed pre-fix, не связаны).

**Разблокированы**: шаг 4.4 (TLS path на единый `_EX` API), шаг 9 (kTLS).

### ~~Шаг 4.1 — Telemetry/CoDel/drain gates на zend_hrtime stamps~~ ✓ (uncommitted)

Сделано во working tree. `http_server_object` получил поле
`sample_stamps_enabled`, выставляется на старте по правилу
`(codel_target_ns != 0) || telemetry_enabled` (drain не нужен — у него
свой fallback к свежему `zend_hrtime`). Публичные хелперы
`http_server_sample_stamps_enabled()` / `http_server_count_request()`
вынесены в `php_http_server.h`. `http_server_on_request_sample` больше
не делает `total_requests++` — счётчик бампается отдельным дешёвым
вызовом, безусловно. Все 3 hot-path сайта (`http_connection.c`,
`http2_strategy.c`, `http3_dispatch.c`) гейтят `enqueue_ns`/`start_ns`/
`end_ns` + `on_request_sample` через флаг.

Эффект на минимальном конфиге (CoDel off, telemetry off): минус 3
`zend_hrtime` per request. Бенч пока не прогонялся.

### ~~Шаг 4.2 — Inline `on_request_sample`~~ ✓ (uncommitted)

Сделано не через перенос всего `struct http_server_object` (потянуло бы
цепочку internal headers — `conn_arena_t`, `http_log_state_t`,
`tls_context_t`), а точечно. На горячий путь приходились две out-of-line
функции: `http_server_sample_stamps_enabled` (3 сайта × 2 раза) и
`http_server_count_request` (3 сайта). Сделал слайс-based:

- В `http_server_view_t` добавлено `bool sample_stamps_enabled`,
  выставляется на старте сервера.
- В `http_server_counters_t` перенесено `uint64_t total_requests` (было
  отдельным полем `http_server_object`).
- Обе функции стали `static zend_always_inline` в `php_http_server.h`,
  принимают `view*` / `counters*` соответственно.
- Все 6 hot-path вызовов в `http_connection.c`/`http2_strategy.c`/
  `http3_dispatch.c` переведены на `conn->view` / `conn->counters`.
- Само `on_request_sample` оставлено out-of-line — оно идёт ТОЛЬКО когда
  stamps включены (после inline-гейта), плюс CoDel-trip ветка вызывает
  `http_server_pause_listeners` который не-hot-path.

Эффект: 9 indirect-call'ов в минимальном конфиге заменены на чтение
поля + сравнение. Компилятор видит, что после `if (!stamps) return`
никакой трафик не уходит в out-of-line — мёртвый код устранён по сайту
вызова.

Бенч пока не прогонялся (микро-эффект, нужны медианы). Все TLS/H1/H2
phpt зелёные кроме двух пре-existing'ов (h1/005, h2/009-h2spec).

### ~~Шаг 4.3 — `CLOCK_MONOTONIC_COARSE` inline хелпер~~ ✓ (uncommitted)

`http_now_coarse_ns()` добавлен в `php_http_server.h` как `static
zend_always_inline`. На Linux идёт через `clock_gettime(CLOCK_MONOTONIC_COARSE)`,
на остальных платформах fallback на `zend_hrtime()`. Заменены три
drain-сайта:

- `http_connection.c:1670` — H1 dispose drain fallback, когда
  stamps off (минимальный конфиг).
- `http2_strategy.c:655` — H2 commit drain decision (раз на conn —
  guard'ится `drain_submitted`).
- `http_server_class.c:768` — `trigger_drain` cooldown check.

Стэмпы CoDel/telemetry (`enqueue_ns`/`start_ns`/`end_ns`) НЕ тронуты —
ns-точность нужна для CoDel-окна.

Эффект (perf -F999 / wrk -t4 -c64 -d18s):
- vdso_clock_gettime: 2.29% → 2.21% (−0.08%)
- http_server_should_drain_now: 0.49% → 0.43% (−0.06%)
- http_handler_coroutine_dispose: 1.25% → 1.09% (−0.16%)

Чистый выигрыш ≈ 0.30% CPU. RPS — wash (внутри WSL2 шума, σ ≈ 7k).

### Шаг 4.4 — TLS path на единый fire-and-forget API

`http_connection_tls.c:535` имеет собственный fire-and-forget mechanism (persistent send-completion callback, stash `cb->write_buf` + `cb->write_req`). Заменить на `ZEND_ASYNC_IO_WRITE_EX` с `efree` free_cb. Удалить связанные поля из `http_connection_tls_t`.

Решение 2026-05-02: пропускаем — перфо-выигрыш ≈ 0%, чистый рефакторинг,
плюс TLS handshake сейчас всё равно сломан. Имеет смысл только после
починки шага 0 и в рамках общего пересмотра TLS-стека.

### Шаг 5 — Coroutine context pool

Per-thread freelist для `async_coroutine_t`, per-conn reuse `http1_request_ctx_t`, переиспользование HttpRequest/HttpResponse zval'ов. Часть оставшихся 2.21% `_emalloc/_efree`.

### ~~Шаг 6 — `zend_get_executed_filename_ex` с горячего пути~~ ✓

Сделано (`7d499d4`). Заменили `call_user_function(NULL, NULL, …)` на
`zend_call_function(&fci, &handler->fci_cache)` во всех трёх dispatch
paths (H1/H2/H3). Кешированный `fci_cache` уже был в `zend_fcall_t` —
populated при `addHttpHandler` через `Z_PARAM_FUNC`. Эффект:
`zend_get_executed_filename_ex` 0.28% → 0.12%, `zend_is_callable_ex`
ушёл из топа.

### Шаг 7 — HTTP/2 hot-path

Профилирование h2load после применения предыдущих шагов. Возможно нужен per-stream output buffer pooling.

### Шаг 8 — HTTP/3 hot-path

QUIC over UDP. Добавить fire-and-forget API для UDP send (или унифицировать с существующим UDP_REQ pattern). Рассмотреть UDP_SEGMENT (GSO).

### Шаг 9 — TLS оптимизация

kTLS, `SSL_sendfile` для больших ответов, меньше копирований между ring buffer'ами.

### ~~Шаг 10 — Vector-write (iovec) для headers + body~~ ✓

Сделано (`cba6418` server, `5080a77` ext/async, `a0d1458fb3` php-src).
API стал `ZEND_ASYNC_IO_WRITEV(io, bufs, nbufs)` — массив owned
`zend_string*` (refcount передаётся reactor'у, free_cb не нужен).
В hot-path HTTP/1 dispose добавлен threshold-branch: body < 1 КиБ
идут через legacy concat-формат (защита от регресса на «hello world»),
≥ 1 КиБ — через writev. См. `docs/PERF_2026_05_02_STEP_10.md` —
A/B показывает +18% rps на /64k, +24% rps на /256k, без регресса на
маленьких. TLS path остаётся на single-buffer (encrypt ring требует
contiguous payload).

### Шаг 11 — Zero-copy для больших ответов

`MSG_ZEROCOPY` или `io_uring SEND_ZC` для ответов > 16 КБ. Threshold-based.
