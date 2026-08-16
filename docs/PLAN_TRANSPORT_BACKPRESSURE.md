# PLAN: Transport-level outbound backpressure (WS = потребитель)

> ⚠️ ВРЕМЕННЫЙ рабочий файл. Удалить в конце работы.
> Ветка: `2-websocket-support-rfc-6455`. Контекст: ревью WebSocket вскрыло, что
> исходящая очередь не ограничена → медленный клиент = безлимитный рост памяти.

## ✅ ИТОГ (реализовано + проверено)

**Сделано (WS-слой backpressure поверх транспортного предиката, под гейтом `stream_write_buffer_bytes==0`):**
- транспортный предикат H1: `http_connection_outbound_pending_bytes/over_highwater` + low-water + `on_outbound_drain` хук (core, аддитивно);
- `ws_session_over_highwater` = очередь wslay (`wslay_event_get_queued_msg_length`, точный байт-gauge) ⋁ H1 out_pending;
- `ws_session_wait_writable` (suspend на drain-event + waker-таймаут, как `Async\delay` — leak-free) → OK/TIMEOUT/CLOSED;
- `send()` блокирующий: над HIGH → suspend → drain ⇒ OK / таймаут ⇒ `WebSocketBackpressureException` / closed ⇒ `WebSocketClosedException`;
- `trySend()`/`trySendBinary()` non-blocking (`bool`, не суспендят; флаш через internal-путь);
- **UAF-фикс**: пин `conn->handler_refcount` на время флаша + `destroy_pending` в dispose — иначе teardown освобождал wslay-ctx под припаркованным флашером (valgrind-подтверждён UAF, исправлен);
- стаб + arginfo (`trySend`/`trySendBinary`), `WebSocketBackpressureException` теперь живой.

**Проверено:** websocket **24/24**; регресс core+h1+h2 **104/106** (0 fail); valgrind на 023+024 — **0 ошибок, 0 утечек**. Сборка без warnings.

**Тесты:** `023-backpressure-trysend` (trySend → BUSY, non-blocking), `024-backpressure-send-blocks` (send суспендится над HIGH и завершается после дренажа).

**Осталось (follow-up, НЕ блокеры):**
- high-water для **wss(TLS)** и **WS-over-H2** (сейчас предикат H1-only; у флашера там backpressure уже есть через TLS-suspend / H2-кольцо, но точный байтовый учёт BIO/ring — отдельно);
- автоматический phpt на сам `WebSocketBackpressureException` (таймаут-путь): требует реально застрявшего флашера (spawned writer + 8MB stuck send), что на CLI оставляет teardown-артефакт отменённого libuv-хэндла (valgrind-clean на process exit, но zend_mm флагает на request-shutdown). Путь проверен вручную gdb+valgrind (0 ошибок), но в авто-suite не вынесен;
- расширить транспортный примитив на H1-streaming + единый `write/try_write` (Шаги 2/4 ниже) — для не-WS потребителей.

---

## 0. Главное решение (направление)

Backpressure — свойство **транспорта** (тормозит сокет), НЕ WebSocket.
Лимит исходящих байт + сигнал «занято» живут на **connection/stream-уровне**;
WS, H1-streaming, H2 — все **потребители** одного примитива. Никакого
`ws_*`-счётчика. H2 уже сделан правильно — это эталон, к которому подтягиваем H1.

---

## 1. Текущее состояние (что нашли по коду)

| Путь | Ограничен? | Механизм / ref |
|---|---|---|
| **H2 stream / WS-over-H2** | ✅ да, настраиваемо | chunk-ring + `stream_write_buffer_bytes` high-water; `h2_stream_append_chunk` **суспендит** продьюсера (`src/http2/http2_strategy.c:1633,1714`) |
| **H1 streaming** (`HttpResponse::send()`) | ⚠️ да, но неявно | «kernel send buffer» + суспендящая запись; НЕ tunable байт-кап, НЕТ `try_write` (`src/http1/http1_stream.c:83,143,176`) |
| **H1 batched/coalesce** (`out_pending_append`) | ❌ **нет** | растёт `erealloc ×2`, без потолка (`src/core/http_connection.c:1505-1523`); вход через `http_connection_send_batched` (`:1636`, ветка `out_in_flight` `:1648-1652`) |

Связанные факты:
- `stream_write_buffer_bytes` читается **только в H2** (`include/php_http_server.h:197,791`; H1 не использует).
- WS internal-sends (PING/PONG/CLOSE из event-loop) идут через `ws_h1_send_internal` → `http_connection_send_batched` → **безлимитный** `out_pending` (`src/websocket/ws_session.c:112-124`).
- WS не-флашер-продьюсер кладёт в очередь wslay **без лимита** (`ws_do_send`, `src/websocket/php_websocket.c:728`); flush сейчас синхронный per-`send()` (`:742`).
- `WebSocketBackpressureException` **сейчас не бросается нигде** (мёртвый, `php_websocket.c:979`) — станет живым через `send()`.
- Уже есть: `conn->write_timeout_ms` / `deadline_ms` / `write_timed_out` + `deadline_tick` (`src/core/http_connection.h:209,292,313`) — это **транспортный write-таймаут** (флашер застрял в сокете), НЕ backpressure-таймаут.
- Inbound backpressure — отдельный известный TODO (`src/http1/http_parser.c:794`, issue #26). В этот план НЕ входит.

---

## 2. Целевая модель

### 2.1 Два таймаута — НЕ путать
- **write_timeout (транспорт):** флашер застрял, записывая в сокет → закрыть conn. Уже есть.
- **backpressure-wait (submit):** продьюсер ждёт места в очереди → отдельная величина. Истёк, conn жив → `WebSocketBackpressureException`.

### 2.2 Watermark (hi/lo, в байтах)
- `HIGH` — включить затор; `LOW` — разбудить спящих. Гистерезис против трешинга.
- Замер — на **одной** куче (где реально копится), потому что flush «вежливый»:
  перекладывает в транспорт только пока тот принимает, остаток оставляет → счётчик честный.
- **Валидация:** `LOW < HIGH` строго (иначе спящий не проснётся → вис).

### 2.3 Flush — переиспользуем существующее, свою микротаску НЕ пишем
- **H2:** `h2_session_schedule_emit` + кольцо — уже есть, WS-over-H2 уже через это.
- **H1:** существующий batched-writer + дренаж по write-ready колбэку соединения.
- Роль flush-драйвера: «прийти потом и дошлёть оставленное в очереди» (write-ready колбэк).

### 2.4 Единый примитив
`write(...) ` (блокирующий, суспендит над HIGH) и `try_write(...) → OK | WOULD_BLOCK`
(не суспендит). На него садятся WS, H1-streaming, H2.

### 2.5 WS API поверх примитива (без своей backpressure-логики)
- `send(): void` — блокирующий `write`: под HIGH кладёт и уходит; над HIGH **спит** до LOW;
  ждал дольше backpressure-таймаута (conn жив) → `WebSocketBackpressureException`;
  conn закрылся за время ожидания → `WebSocketClosedException`.
- `trySend(): …` — `try_write`: над HIGH **сразу** `BUSY`, не суспендит; под HIGH — отдал + `SENT`.

---

## 3. Шаги реализации (по порядку)

### Шаг 1 — H1: ограничить `out_pending` + статус
- [x] **Транспортный примитив (accessor + high-water предикат), гейтнут на `stream_write_buffer_bytes`.**
      `http_connection_outbound_pending_bytes()` + `http_connection_outbound_over_highwater()`
      в `src/core/http_connection.{c,h}`. Аддитивно, 0=disabled → ноль регресса. **Собирается.**
- [ ] LOW-отметка: per-conn drain-event + пробуждение спящих в `http_send_batched_completion_cb`
      (`src/core/http_connection.c:1525`), когда `out_pending_len` просел до LOW.
- [ ] Блокирующий путь: продьюсер над HIGH делает `resume_when` на drain-event,
      борт по backpressure-таймауту.
- [ ] Non-blocking путь для `trySend`: предикат уже есть — продьюсер сам решает BUSY до handoff
      (без передачи владения буфером).

### Шаг 2 — Единый примитив на транспорте
- [ ] Объявить `http_conn_try_write(conn, buf, len) → OK|WOULD_BLOCK` и блокирующую обёртку.
- [ ] H2: смаппить на `h2_stream_append_chunk` (уже возвращает статус `HTTP_STREAM_APPEND_*`).
- [ ] H1: смаппить на ограниченный `send_batched` из Шага 1.
- [ ] (опц.) H1-streaming подтянуть к тому же примитиву (сейчас kernel-backpressure).

### Шаг 3 — WS садится сверху
- [ ] `ws_h1_send` / `ws_h1_send_internal` / `ws_h2_send*` → вызывают единый примитив
      (`src/websocket/ws_session.c:106-129`, `src/http2/http2_strategy.c` ws_h2_send*).
- [ ] Убрать синхронный flush-per-`send()` (`ws_do_send`, `php_websocket.c:735-753`):
      enqueue + flush через примитив/колбэк.
- [ ] `WebSocket::send()` → блокирующий write; над HIGH suspend; таймаут → `WebSocketBackpressureException`; closed → `WebSocketClosedException`.
- [ ] `WebSocket::trySend()` → `try_write`; над HIGH → `BUSY`.

### Шаг 4 — Knobs / конфиг
- [ ] Расширить `stream_write_buffer_bytes` на H1 (сейчас H2-only) — один транспортный HIGH.
- [ ] LOW: решить — отдельный knob или производный (см. Открытые вопросы).
- [ ] Backpressure-wait таймаут — отдельный live-knob (НЕ переиспользовать write_timeout).
- [ ] Валидация `LOW < HIGH` в сеттерах → `HttpServerInvalidArgumentException`.

### Шаг 5 — Stub / API truth-up + тесты
- [ ] Добавить `trySend()` в `stubs/WebSocket.php` + arginfo.
- [ ] Привести docblock `send()` к реальному поведению.
- [ ] `WebSocketBackpressureException` — теперь живой.
- [ ] Тесты:
  - [ ] медленный клиент → `send()` спит → по таймауту `WebSocketBackpressureException`, conn жив;
  - [ ] `trySend()` над HIGH → `BUSY`, не блокирует;
  - [ ] broadcast с одним медленным клиентом не стопорит остальных;
  - [ ] H1: `out_pending` не растёт безлимитно под медленным потребителем.

---

## 4. Открытые вопросы (решить до/по ходу)
1. **`trySend` контракт:** плоский `bool` vs enum `WebSocketSendResult {SENT, BUSY}` + `WebSocketClosedException` на закрытом. (не решено)
2. **Форма knob'ов:** один `stream_write_buffer_bytes` (HIGH) + производный/отдельный LOW, vs два явных транспортных `*_high_bytes`/`*_low_bytes`. (склонялись к двум явным)
3. **Backpressure-wait таймаут:** имя + дефолт.
4. **H1-streaming:** рефакторить на общий примитив или оставить kernel-backpressure как есть.

## 5. Вне scope
- Inbound backpressure (`llhttp_pause`, issue #26).
- Мёртвые WS-knobs `ws_max_frame_size` / `ws_pong_timeout_ms` (чинятся/удаляются отдельно).
- Удаление `int` из `WebSocket::close(WebSocketCloseCode|int)` (отдельная заметка).
