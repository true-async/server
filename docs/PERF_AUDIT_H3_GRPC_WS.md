# Perf-аудит: путь запроса HTTP/3 (reactor→worker), gRPC, WebSocket

Дата: 2026-07-09. База: main @ 9d7d840.
Метод: ручная трассировка кода (без профайлера) по файлам
`src/http3/*`, `src/core/{worker_dispatch,worker_inbox,thread_mailbox,thread_queue,response_wire,reactor_pool}.*`,
`include/core/stream_credit.h`, `src/grpc/*`, `src/websocket/*`, `src/http_request.c`, `src/http_server_class.c`.

Статус пунктов: `[ ]` — не обсуждён, `[FIX]` — принят к исправлению, `[WONTFIX]` — отклонён.

---

## 1. Карта HTTP/3 (reactor-pool режим): датаграмма → воркер → ответ

### Ingress (реактор-тред)

| Этап | Аллокации | Копии данных | Syscalls / тяжёлое |
|---|---|---|---|
| `recvmmsg` batch (`http3_listener_poll_cb`) | 0 (стек) | kernel→user | 1 syscall / ≤10 дгр (+GRO) |
| `pkt_decode_version_cid` + `conn_map` lookup | 0 | — | hash 8–20 байт/дгр |
| `ngtcp2_conn_read_pkt` | внутр. ngtcp2 | AEAD decrypt | крипто, неизбежно |
| заголовки (`h3_recv_header_cb`) | **2 malloc/заголовок** (persistent name+value) + HT + bucket; method/uri ещё 2 | QPACK rcbuf → zend_string | ~22 malloc на 10 заголовков |
| тело (буферное) | smart_str рост (ZMM) → **полная persistent-копия** в `http3_finalize_request_body` | **копия ×2 уже на реакторе** | — |
| dispatch → inbox (`worker_inbox_post`) | слаб-пул стрима: pop + memset ~900Б | — | CAS `count` + eventfd — **хоп №1** |

### Worker

| Этап | Аллокации | Копии |
|---|---|---|
| `worker_dispatch_request` | HttpRequest-объект + heap-zval-врапер (emalloc→сразу efree), `ecalloc ctx`, HttpResponse, корутина+scope | — |
| хендлер: `getHeaders()` | **полная пересборка HT + 2 ZMM-аллока/заголовок на каждый вызов** (кэша нет) | заголовки: копия №2 |
| хендлер: `getBody()` | deep-copy persistent→ZMM **на каждый вызов** | тело запроса: копия №3 |
| рендер FULL (`worker_render_response`) | `response_wire` calloc + arena realloc-удвоения + массив заголовков | все заголовки + всё тело → arena (**копия ответа №1**) |
| sink → реактор (`post_exec`) | 0 (cmd по значению) | — | CAS + eventfd — **хоп №2** |

### Обратный путь (реактор)

`http3_stream_submit_response_wire`: тело → `zend_string_init` (**копия №2**) → nghttp3 QPACK-encode
(заголовки копия №3) → `ngtcp2_conn_writev_stream` в `batch_buf` (**копия тела №3**) → `sendmsg`/GSO
(**копия №4, kernel**). Деструктор запроса на воркере постит release слота через cmd-mailbox — **хоп №3**.

### Итог на один GET+ответ (reactor-режим)

- ~30–45 heap-аллокаций (доминируют заголовки: 2 malloc на реакторе + 2 ZMM на воркере на каждый);
- тело ответа копируется **4 раза**, тело запроса — **3 раза до хендлера**;
- **3 кросс-тредовых пробуждения** (inbox → wire → consumed-release).

Локальный (не-pool) режим заметно чище: `zend_string_copy` тела ответа (addref, zero-copy),
нет wire, нет хопов.

---

## 2. Карта gRPC (поверх H2/H3 + reactor)

- Классификация: hash-find `content-type` + strncasecmp — дёшево; `http_protocol_pick_handler`
  вызывается дважды (dispatch и entry) — мелочь.
- `readMessage` буферный: `zend_string_init` на каждое сообщение (копия). Норма.
- **`readMessage` стриминговый — плохо**: чанки конкатенируются в `grpc_reassembly`, и после каждого
  извлечённого сообщения при приходе нового чанка `memmove` всего хвоста в начало
  (`src/http_request.c:323`). При бурсте из N мелких сообщений — O(N × буфер), квадратичное поведение.
- **`writeMessage`**: сообщение → `grpc_frame_message` (аллок 5+len, копия №1) → `append_chunk` →
  persistent-копия (№2, reactor-режим) → wire calloc + mailbox + apply + `resume_stream` + полный
  `drain_out`. **Каждое мелкое сообщение = отдельный wire-раундтрип без батчинга** — при
  высокочастотном стриминге пер-сообщенческий оверхед доминирует.
- grpc-web-text: декод тела блоками по `=`-паддингу — отдельный `php_base64_decode` + append на
  каждый фрейм (лишняя копия на фрейм); результат кэшируется в `grpc_text_body` — приемлемо.
  Ответ: +1 base64-копия (×1.33 размера).

---

## 3. Карта WebSocket (H1/wss)

Всё на одном треде — хопов нет (хорошо). Но:

- Ingress: read-буфер → wslay внутренняя пересборка (копия №1) → `zend_string_init` (№2)
  [+ PMCE inflate с realloc-ростом] → `emalloc` узла FIFO. 2 копии + 2 аллока/сообщение — терпимо.
- **Egress без сжатия: 3 копии** — payload → `wslay_event_queue_msg` (внутренняя копия №1) →
  `ws_session_send_callback` в `send_buf` smart_str (№2) → `send_internal` `emalloc`-копия в
  batched writer (№3). С PMCE — 4. Фрагментированный путь добавляет ещё полную копию payload
  в `frag_source`.
- **Главная дыра — входной FIFO не ограничен**: `ws_session_on_msg_recv_callback` кладёт
  сообщения в `recv_head/tail` без cap и без паузы чтения. `ws_max_message_size` ограничивает
  размер одного сообщения, но не их число. Медленный/не вызывающий `recv()` хендлер + активный
  клиент = неограниченный рост памяти. Backpressure сделан только на исходящем направлении.

---

## 4. Кэш-линии и межтредовая грязь

1. **`http_server_counters_dummy` — общая горячая строка всех реакторов.** В reactor-режиме
   `server_obj` лиснера = NULL ⇒ `c->counters` указывает на глобальный dummy;
   `http_server_on_stream_send`/`on_request_dispatch` — обычные (не атомарные) `++` из нескольких
   реакторных тредов в один глобальный struct: пинг-понг кэш-линии на каждый чанк/запрос +
   формально data race (UB). Нужен per-thread counters-slice.
2. **`thread_mpsc_s.count`** — точный атомарный счётчик поверх moodycamel: CAS каждого продюсера +
   `fetch_sub` консьюмера на одной строке — возвращает сериализацию, которую moodycamel избегает.
   `worker_inbox_depth()` читается реакторами при каждом решении о спилле — постоянные cross-core
   трансферы. `capacity` (read-only) лежит на той же строке.
3. **`stream_credit_t`** — 40 байт из calloc, без выравнивания на 64: `acked` (пишет реактор на
   каждый ACK) + `waker_busy` (2 RMW на wake) + опрос `acked/dead` воркером — одна линия; два
   credit'а разных стримов могут делить линию. `stream_credit_clear_waker` крутит **голый spin
   `while(busy){}` без cpu_relax/pause** — при вытеснении реактора внутри `trigger()` воркер жжёт ядро.
4. Безусловный `trigger()` на каждый `thread_mailbox_post` — атомарный RMW на pending-слове
   uv_async при каждом посте (фикс lost-wakeup корректный, но edge-оптимизация выброшена вместе
   с багом, а не починена).

---

## 5. Стек, алгоритмы, структуры

- **Стек `poll_cb` — комментарий врёт в 15 раз**: «Stack buffers (~16 KiB), zero heap»
  (`http3_listener.c:473`), но с `UDP_GRO` `bufs[10][24576]` = **240 КиБ**, плюс вложенный
  `flush_dirty → drain_out` с `batch_buf` 96 КиБ ⇒ ~340 КиБ глубины на реакторном треде.
- **`getenv()` на пути accept**: `PHP_HTTP3_BENCH_FC` дважды + `PHP_HTTP3_IDLE_TIMEOUT_MS` +
  `PHP_HTTP3_DISABLE_RETRY` — 4 линейных скана environ на каждый Initial. Кэшируется одной статикой.
- **Таймер на каждую итерацию ожидания**: `worker_stream_wait_credit` и `h3_stream_append_chunk`
  создают новый timer-event на каждый цикл suspend (аллокация + uv_timer) вместо переиспользования
  multishot-слота (как сделано для conn-таймера).
- `conn_list` / `unmark_flush` — O(N) unlink; при высоком churn тысяч соединений reap суммарно O(N²).
- **Асимметрия форвард/реверс-каналов**: обратный путь — плоский `response_wire` (arena, 1 malloc +
  рост), прямой — полный `http_request_t` с persistent HashTable и 2 malloc/заголовок, которые
  воркер всё равно пере-копирует в ZMM. Плоский request_wire с ленивым парсом на воркере срезал бы
  половину аллокаций пути.
- `getHeaders()` / `getBody()` без кэша результата — каждый вызов платит полную пересборку/копию.
- **Повторяющиеся заголовки затирают друг друга в H2 и H3** (`zend_hash_update`, last-wins), тогда
  как H1 склеивает дубли запятой по RFC 7230 (`http_parser.c:399`). Проверено в обоих файлах:
  `http2_session.c:131` и `http3_callbacks.c` (`h3_store_header_value`). Корректность + расхождение
  поведения между транспортами.
- **H3 не использует интернированные имена заголовков**: H2 идёт через `http_known_header_lookup`
  (ноль аллокаций на common-имена: host, content-length, ...), H3 `h3_store_header_value` всегда
  делает `zend_string_init` имени — лишний malloc на каждый стандартный заголовок.

Сделано хорошо (не трогать): recvmmsg/GSO/GRO, dirty-list с отложенным flush, слаб-пул стримов,
multishot rearm conn-таймера, кэш inet_ntop по peer, errqueue-drain по флагу, moodycamel как основа
каналов, admission-гейты (Retry / per-peer budget / max_conns).

---

## 6. Топ проблем по приоритету

Уверенность: ★★★ — подтверждено чтением кода в обе стороны; ★★ — факт по коду верен, но
магнитуда/эксплуатируемость не измерена; ★ — дизайн-гипотеза.

1. `[FIX]` ★★★ **WS: неограниченный входной FIFO — ИСПРАВЛЕНО** (`bda4526`). Байтовый учёт FIFO,
   cap = 8× ws_max_message_size; переполнение → close 1013 + teardown через feed()→-1; pop
   возвращает бюджет. Тест websocket/035. (Пауза чтения сокета невозможна без ABI — read
   multishot и не останавливается; жёсткий backstop выбран осознанно.)
2. `[FIX]` ★★★ **Общий dummy-counters между реакторными тредами — ИСПРАВЛЕНО** (`b42be59`).
   Каждый reactor-spawn лиснер несёт встроенный counters-слайс, трогаемый только своим тредом;
   dummy остаётся для truly-unsupervised коннекшенов.
3. `[FIX]` ★★★ **Копии тел в reactor-режиме — СРЕЗАНО ПО КОПИИ В ОБЕ СТОРОНЫ** (`d9dd179`,
   `3bb493a`, + кэш getBody `d4ee880`). Ответ: wire несёт тело как persistent zend_string, реактор
   адоптирует ref (было arena-копия + re-init; стало 4→3 копии, из них 2 неустранимы — ngtcp2
   пакетизация и kernel). Запрос: тело собирается persistent с первого байта, finalize отдаёт
   без копии (3→2 до хендлера); повторные getBody() берут кэш.
4. `[WONTFIX]` **Кросс-тредовые хопы на запрос** — перепроверено по коду: 3 сообщения / 2 канала /
   обычно 2 eventfd-записи (uv_async коалесцирует на 0→1; wire+release с одного треда почти всегда
   в одну запись; inbox-дренаж батчится по 64). Consumed-release семантически обязателен (borrow-ref
   живёт до смерти PHP-объекта HttpRequest — юзер может сохранить `$request`). Остаток цены —
   единицы µs латентности одиночного запроса, неустранимый налог reactor/worker-сплита; для тех,
   кому он не нужен, есть локальный режим без пула.
5. `[FIX]` ★★★ **gRPC: memmove-компакция — ИСПРАВЛЕНА** (`c58c262`). Батчинг мелких сообщений
   в стриминге — открыт (дизайн-гипотеза, см. низ файла).
   **Флак grpc/013 — БАГ НАЙДЕН И ИСПРАВЛЕН** (`3b7f8f4`): дедлок «complete-before-upgrade».
   Если весь аплоад принят и финализирован буферным путём ДО первого слота хендлер-корутины
   (30+ read_cb подряд в одном тике), ленивый `body_upgrade_to_stream` включал стриминг на уже
   завершённом запросе: очередь пуста, EOF от END_STREAM уже потреблён буферной веткой —
   `readMessage()`/`readBody()` спали до смерти коннекта. Уязвимы были все три транспорта
   (общий хук). Фикс: гард `req->complete` в трёх upgrade-функциях; потребители падают в
   корректную буферную ветку. Детерминированный регресс grpc/017 (delay перед первым read;
   FAIL до фикса / PASS после). Методика поимки: fprintf-трасса до /tmp + отключение retry в
   run-tests (ключевые слова curl-verbose «timed out» триггерили is_flaky_output и retry
   молча съедал diff — в форензике 013 они теперь манглятся).
6. `[DEFERRED]` ★☆☆ **Atomic-счётчик поверх moodycamel — возможно, вообще не проблема.** Обсуждено:
   контеншн переоценён — продюсеры одного inbox'а это 2–8 реакторов, и sticky-хоминг сводит
   типичный случай к одному пишущему; кросс-реакторные CAS только при спилле. Кап (1024) НЕ
   размягчать: он — граница памяти воркера (элемент держит persistent-тело до 16 МБ), а
   аппроксимация даёт и перелёт, и ложные RESET. Если когда-нибудь трогать, то только бесплатное:
   паддинг `count` на отдельную кэш-линию (убирает false sharing с `capacity`/внутренностями
   очереди без смены семантики) + relaxed depth-load для spill-решений. До бенча не приоритизировать.
7. `[WONTFIX]` **WS egress — перепроверено, не проблема.** Продюсерский путь `$ws->send()`
   (plaintext, без сжатия) = **2 копии**: (1) `wslay_event_queue_msg` malloc+memcpy — подтверждено
   кодом wslay (`wslay_event.c:222`), это пол его API; (2) коалесценция header+payload в `send_buf`
   — оправдана (один write вместо двух). Дальше zero-copy: `send_raw` отдаёт `send_buf` прямо в
   uv_write и ждёт завершения. Internal-путь (контрол-фреймы ≤125Б) = 3 копии — не имеет значения.
   PMCE +1, фрагментация больших сообщений +1 (`frag_source`). Побочное наблюдение: последовательный
   отправитель платит suspend/resume + write-syscall на каждое сообщение (нет межсообщенческой
   коалесценции у одного продюсера) — осознанный дизайн backpressure'а, трогать не надо.
8. `[FIX]` ★★★ **Заголовки: H2/H3 затирали дубли + H3 без интернированных имён — ИСПРАВЛЕНО**
   (`dd80a47`). Общий `http_request_store_header()` (src/http_request.c): дубли склеиваются по
   порядку, `cookie` через `"; "` (RFC 9113 §8.2.3 / RFC 9114 §4.2.1), остальное через `", "`
   (RFC 9110 §5.3); H1/H2/H3 на одном хелпере, H3 переведён на `http_known_header_lookup`.
   Тесты: h1/026, h2/029.
9. `[FIX]`/`[WONTFIX]` **Мелочи** (`d4ee880`): стейл-коммент про стек poll_cb исправлен; getenv-
   хатчи закэшированы; pause/yield в spin `clear_waker`; кэш `getHeaders()`/`getBody()` после
   req->complete. WONTFIX: таймер-аллок на итерацию credit-wait (waker_new_with_timeout внутри
   тоже создаёт таймер — выигрыш нулевой, а аллокация происходит перед suspend'ом на миллисекунды
   — шум); двойной `pick_handler` в gRPC-dispatch (кэшировать fcall через enqueue-гэп небезопасно
   при hot-reload, а цена — два hash-lookup на запрос).

Дизайн-гипотезы (★): плоский request_wire вместо persistent HashTable на прямом пути (экономия
зависит от того, читает ли хендлер все заголовки); батчинг wire'ов для мелких gRPC-сообщений.

## 7. Попутные находки CI (2026-07-10)

- `[FIX]` **Autobahn nightly был красным с первого full-рана (07-02), три слоя:** (1) в образе не
  было zlib-заголовков (ubuntu:24.04 `libxml2-dev` больше не тянет `zlib1g-dev`) → compression
  тихо выключен → PMCE не негоциировался → все 216 кейсов 12.*/13.* UNIMPLEMENTED (`4a08704`);
  (2) `SERVER_REF=main` не менял строку docker-инструкции → GHA-кэш вечно отдавал замороженный
  checkout — nightly тестировал несвежий сервер; фикс = SHA вместо ref (`5e3fb72`); (3) свежий
  main не собирается на NTS (`--disable-zts` в образе): `tsrm_mutex_*` в worker_registry.c —
  ZTS-only; образ переведён на ZTS как в build-linux.yml.
- `[WONTFIX→гард]` **NTS не поддерживается по дизайну** (тред-пулы воркеров/реакторов, TSRM) —
  «несборка под NTS» не баг: NTS-таргета нет нигде (CI весь ZTS; единственный `--disable-zts`
  был в кривом Autobahn-образе, исправлен). Добавлен fail-fast гард в configure (config.m4 +
  config.w32): внятная ошибка вместо криптичного `tsrm_mutex_*` глубоко в компиляции.
- `[FIX]` **fuzz-линк**: новый вызов из parser-TU в http_request.c → weak stub (`9ea9e2c`);
  правило уже было в fuzz_stubs.c, теперь соблюдено для `http_request_store_header`.
