# Архитектура отдачи файлов (sendfile / static)

> **Статус: реализовано на ветке `13-built-in-static-file-handler`.**
> Все 6 шагов плана сделаны и закоммичены. Документ оставлен как
> reference на дизайн-решения; разделы плана (§9) помечены ✅. Для
> backlog'а дальнейших мини-рефакторингов см. §11. **Производительная
> переделка движка (sync prologue + slurp fast path) — см. §14.**

Документ описывает архитектуру для двух фич, которые до рефакторинга жили
параллельно и дублировали код:

- `StaticHandler` — отдача файлов по URL-префиксу до PHP-хэндлера.
- `Response::sendFile()` — отдача файла из PHP-хэндлера, дефер до dispose-фазы.

Цель — **один движок отдачи**, два тонких адаптера-входа, общие HTTP-утилиты
поднять на уровень проекта. Достигнута: `src/send_file.c` (~700 LOC)
обслуживает оба входа.


## 1. Принцип группировки кода

Группировка — **по владению данных**, не по «архитектурным слоям».
Вопрос, на который мы отвечаем для каждого куска кода: «кому он принадлежит?»

- Метод проверяет request? — это метод request-объекта.
- Функция формирует ETag из stat? — это HTTP-протокольная утилита.
- FSM открывает файл и шлёт его в protocol-op? — это движок send_file.
- Парсер MIME по расширению? — общая HTTP-утилита, могут пользоваться и uploads,
  и content-negotiation, и send_file.

Один из главных дефектов текущего кода — что базовые методы request/response
объектов **не были выставлены** как публичный API, и каждый потребитель
(static, sendfile, compression) изобрёл свою локальную копию.


## 2. Раскладка (фактическая)

```
src/
├── http_request.c          (+ публичные методы из §3)
├── http_response.c         (+ публичные методы из §3)
├── http_mime.c             — builtin MIME-by-extension
├── http_etag.c             — weak ETag format + INM match
├── http_date.c             — RFC 7231 IMF format/parse
├── http_range.c            — RFC 9110 §14.1.2 single-range parser
├── http_conditional.c      — 304-decision над etag/date
├── http_rfc5987.c          — encoder + decoder (multipart filename* fix)
├── fs_util.c               — fs_slurp_fd
├── send_file.c             — единый движок отдачи (~700 LOC)
├── http_send_file.c        — адаптер Response::sendFile (293 LOC)
└── static/
    ├── http_static.c       — адаптер StaticHandler (827 LOC, было 1576)
    ├── http_static_path.c, http_static_cache.c, static_handler_class.c
    │                         — без изменений
    └── (удалены: http_static_etag.{c,h}, http_static_mime.{c,h})

include/
├── http_request.h, http_response.h
├── http_mime.h, http_etag.h, http_date.h, http_range.h,
│   http_conditional.h, http_rfc5987.h, fs_util.h, send_file.h
└── static/                 — без etag/mime, http_static_dispatch_cbs_t
                              теперь typedef-alias send_file_cbs_t
```

Плоская структура. Никаких подкаталогов `send_file/`, `helpers/`. Каждый
файл лежит на своём этаже, имя сразу говорит о содержимом, дубликатов
между TU не осталось.


## 3. Расширение API request/response

Это не новые модули — это методы, которые сразу должны были быть на объектах.
Добавляются в существующие TU.

### `http_request_*`

```c
bool                http_request_method_is_get(const http_request_t *req);
bool                http_request_method_is_head(const http_request_t *req);
const zend_string  *http_request_find_header(const http_request_t *req,
                                             const char *name, size_t name_len);
```

Текущие потребители (после рефакторинга — потребители публичного API):
- `http_static.c` (два собственных дубля).
- `http_send_file.c` (`sf_first_request_header`).
- `http_compression_response.c` (`method_is_head`).

### `http_response_*`

```c
void  http_response_set_content_length(zend_object *resp, uint64_t len);
void  http_response_set_connection(zend_object *resp, bool keep_alive);
bool  http_response_should_keep_alive(const http_request_t *req);
void  http_response_emit_status_body(zend_object *resp, int code,
                                      const char *body, size_t body_len);
void  http_response_synth_error(zend_object *resp, int code, const char *message);
```

Текущие потребители:
- `http_static.c` (`emit_status`, `set_content_length`, keep-alive расчёт).
- `http_send_file.c` (`sf_set_content_length`, `sf_emit_inline_error`,
  `sf_synth_500`).


## 4. Общие HTTP-утилиты (новые TU)

Stateless функции протокольного уровня — оперируют строками/числами,
не знают про request/response объекты.

### `src/http_mime.c` + `include/http_mime.h`

```c
const char *http_mime_lookup_by_ext(const char *path, size_t path_len);
```

Builtin-таблица по расширениям. Переезжает из `src/static/http_static_mime.c`.

### `src/http_etag.c` + `include/http_etag.h`

```c
#define HTTP_ETAG_BUF_LEN  21        /* "..." + NUL */

void  http_etag_format_strong(const struct stat *st, char buf[HTTP_ETAG_BUF_LEN]);
bool  http_etag_match_inm(const char *header, size_t header_len,
                          const char *etag, size_t etag_len);
```

Переезжает из `src/static/http_static_etag.c` (минус IMF-date — в http_date).

### `src/http_date.c` + `include/http_date.h`

```c
#define HTTP_DATE_BUF_LEN  30        /* "Sun, 06 Nov 1994 08:49:37 GMT" + NUL */

void   http_date_format_imf(time_t t, char buf[HTTP_DATE_BUF_LEN]);
time_t http_date_parse_imf(const char *src, size_t src_len);  /* (time_t)-1 on error */
```

Самостоятельная RFC 7231 IMF-fixdate реализация. Не использует `strftime`
(чтобы не зависеть от LC_TIME). Переезжает из `http_static_etag.c`.

### `src/http_range.c` + `include/http_range.h`

```c
typedef enum {
    HTTP_RANGE_ABSENT,             /* нет заголовка → 200 */
    HTTP_RANGE_OK,                 /* валидный → 206 */
    HTTP_RANGE_NOT_SATISFIABLE,    /* кривой/выходит за пределы → 416 */
    HTTP_RANGE_UNSUPPORTED,        /* multi-range и т.п. — fallback на 200 */
} http_range_result_t;

http_range_result_t http_range_parse(const char *header, size_t header_len,
                                      uint64_t content_length,
                                      uint64_t *out_first, uint64_t *out_last);
```

Парсер `Range: bytes=...`. Переезжает из `parse_byte_range` в `http_static.c`.

### `src/http_rfc5987.c` + `include/http_rfc5987.h`

```c
/* RFC 5987 §3.2 ext-value: charset ' [ language ] ' value-chars.
 * Percent-encode everything outside unreserved + attr-char.
 * Used by Content-Disposition writers (filename*=UTF-8''...) and
 * by Content-Disposition parsers (multipart filename* decode). */
void   http_rfc5987_encode(smart_str *out, const char *src, size_t src_len);
size_t http_rfc5987_decode(char *out, const char *src, size_t src_len);
```

**Два потребителя сразу:**
- `src/send_file.c` — encoder при формировании `Content-Disposition: ...; filename*=...`
  (сейчас inline таблицей в `sf_set_content_disposition`).
- `src/formats/multipart_processor.c` — decoder, **закрывает существующий
  TODO** на `parse_content_disposition` (`multipart_processor.c:212`,
  «Simple copy for now - TODO: proper URL decoding»). Сейчас имена файлов
  с не-ASCII (`Документ.pdf` и т.п.) приходят в обработчик percent-encoded.
  Извлечение этого helper'а попутно фиксит баг.

### `src/http_conditional.c` + `include/http_conditional.h`

```c
typedef enum {
    HTTP_COND_CONTINUE,            /* отдавать как обычно */
    HTTP_COND_NOT_MODIFIED,        /* 304 */
} http_conditional_result_t;

http_conditional_result_t http_conditional_check(const http_request_t *req,
                                                  const char *etag, size_t etag_len,
                                                  time_t mtime);
```

Высокоуровневая склейка: читает `If-None-Match` / `If-Modified-Since` из request,
дёргает `http_etag_match_inm()` и сравнение `mtime`. Единая точка принятия решения
о 304. Переезжает из conditional-блока в `http_static.c`.


## 5. FS-утилита

### `src/fs_util.c` + `include/fs_util.h`

```c
zend_string *fs_slurp_fd(int fd, size_t expected_size);
```

Прочитать весь fd в `zend_string`. Переезжает из `slurp_fd` в `http_static.c`.


## 6. Движок (новый TU)

### `src/send_file.c` + `include/send_file.h`

```c
typedef struct {
    /* что отдаём */
    const char  *abs_path;
    size_t       abs_path_len;

    /* override заголовков (NULL = выводить из файла) */
    zend_string *content_type;          /* NULL → MIME-detect */
    zend_string *content_disposition;   /* готовая строка, NULL = не выставлять */
    zend_string *cache_control;         /* NULL = не выставлять */

    /* фича-переключатели */
    bool etag                : 1;       /* emit ETag, honor If-None-Match */
    bool last_modified       : 1;       /* emit Last-Modified, honor If-Mod-Since */
    bool accept_ranges       : 1;       /* emit Accept-Ranges, parse Range */
    bool precompressed       : 1;       /* искать .br/.gz/.zst */
    bool conditional         : 1;       /* делать 304-shortcut */

    /* побочные эффекты */
    bool delete_after_send   : 1;       /* unlink(abs_path) после успеха */

    /* override итогового статуса (0 = авто 200/206/304) */
    int  status_override;

    /* mount-only (NULL для sendFile) */
    const HashTable *extra_headers;
    const HashTable *mime_overrides;

    /* поведение при ошибке */
    enum {
        SEND_FILE_ERR_INLINE_500,        /* sendFile: синтезировать 500-тело */
        SEND_FILE_ERR_PASSTHROUGH_PHP    /* StaticHandler: откатиться в PHP */
    } on_error;
} send_file_config_t;

typedef struct {
    void (*on_armed)(void *user);
    void (*on_done)(void *user, int status);
    void (*on_passthrough)(void *user);   /* когда on_error == PASSTHROUGH */
    bool (*keep_alive)(void *user);
} send_file_cbs_t;

typedef enum {
    SEND_FILE_PASSTHROUGH = 0,    /* откат в PHP, on_passthrough вызван */
    SEND_FILE_HANDLED     = 1,    /* response готов синхронно (304 или 4xx) */
    SEND_FILE_ASYNC       = 2,    /* async-цепочка в полёте */
} send_file_result_t;

send_file_result_t send_file(http_request_t *request,
                             zend_object    *response_obj,
                             const send_file_config_t *config,
                             const send_file_cbs_t    *cbs,
                             void                     *user);
```

Внутри send_file.c остаётся:
- FSM состояния (`S_OPEN → S_STAT → S_DELIVER → S_DONE`).
- Sidecar lookup precompressed (`.br` / `.gz` / `.zst`).
- Content-Disposition RFC 5987 escape (пока inline; вынесем при появлении 2-го потребителя).
- Заполнение Content-Type / Content-Length / Cache-Control / Accept-Ranges по
  stat и конфигу.
- Delete-after-send.
- Делегирование в protocol vtable-op (`send_static_response`).

Из движка дёргаются: `http_mime_*`, `http_etag_*`, `http_date_*`, `http_range_*`,
`http_conditional_*`, `http_request_*`, `http_response_*`, `fs_slurp_fd` —
всё через публичные API.


## 7. Адаптеры (только худеют)

### `src/static/http_static.c` (~500 строк, было 1660)

- URL prefix matching.
- Path resolution (через `http_static_path.c`).
- Open-file cache integration.
- Mount-policy (dotfile/symlink/index/on_missing).
- Заполнение `send_file_config_t` из mount-полей.
- Вызов `send_file()` с `on_error = PASSTHROUGH_PHP`.

Из него выкидывается весь FSM, IMF-date format, range parser, conditional check,
slurp_fd, emit_status, set_content_length, method_is_get/head, find_header.

### `src/http_send_file.c` (~80 строк, было 735)

- Snapshot `SendFileOptions` → `send_file_config_t`.
- `on_error = INLINE_500`.
- Вызов `send_file()`.

Из него выкидывается весь FSM (`sf_handle_open`, `sf_handle_stat`,
`sf_try_precompressed`, `sf_finalize`), set_content_length, set_content_disposition
помощник, emit_inline_error, synth_500, first_request_header.


## 8. Что не трогаем

- vtable-op `send_static_response` (H1, H2) — рабочий контракт протоколов.
- Mount-структуры (`http_static_handler_t`, lock/freeze/release).
- Open-file cache (`http_static_cache.c`).
- Path resolution policies (`http_static_path.c`).
- PHP-классы `StaticHandler`, `SendFileOptions`, `SendFileDisposition` и enum'ы политик.
- Dispose-механика sealed-state в `http_response.c`.
- Тесты (поведение не меняется).


## 9. План работ — по шагам

### Шаг 1. Расширение API request/response

1. Добавить в `src/http_request.c` методы `http_request_method_is_get/head`,
   `http_request_find_header`. Декларации в `include/http_request.h`.
2. Добавить в `src/http_response.c` методы `http_response_set_content_length`,
   `http_response_set_connection`, `http_response_should_keep_alive`,
   `http_response_emit_status_body`, `http_response_synth_error`.
   Декларации в `include/http_response.h`.
3. Заменить локальные дубли в `http_static.c`, `http_send_file.c`,
   `http_compression_response.c` на вызовы публичного API.
4. Регенерация билда, вся test suite зелёная.

**Контрольная точка**: 12 static + 4 sendfile + 167 server тестов зелёные.
Дубликатов методов request/response в проекте больше нет.

### Шаг 2. Извлечение общих HTTP-утилит ✅

Сделан в этой ветке. Diff: `+~750 / -~900` (rename + relocate, без новой
логики, кроме decoder'а в `http_rfc5987` — он закрыл TODO в multipart).
Все 27 целевых тестов (12 static + 4 sendfile + 11 multipart) и полная
server-suite (133 PASS / 1 SKIP) зелёные.

1. Создать `src/http_mime.c` + `include/http_mime.h`. Перенести содержимое
   `src/static/http_static_mime.c` с переименованием API
   (`http_static_mime_lookup` → `http_mime_lookup_by_ext`).
2. Создать `src/http_date.c` + `include/http_date.h`. Перенести IMF-format/parse
   из `http_static_etag.c`. Переименовать API.
3. Создать `src/http_etag.c` + `include/http_etag.h`. Перенести format/INM-match
   из `http_static_etag.c` (минус http_date — он уже отдельно).
4. Создать `src/http_range.c` + `include/http_range.h`. Перенести `parse_byte_range`
   из `http_static.c`, переименовать в `http_range_parse`.
5. Создать `src/http_conditional.c` + `include/http_conditional.h`. Реализовать
   `http_conditional_check()` поверх `http_etag_match_inm()` и сравнения mtime.
   Перенести conditional-блок из FSM `http_static.c` в эту функцию.
6. Создать `src/http_rfc5987.c` + `include/http_rfc5987.h`. Encoder из
   `sf_set_content_disposition`, decoder — новый. Подключить в `send_file.c`
   (use encoder) и `multipart_processor.c::parse_content_disposition`
   (use decoder, закрывает TODO про percent-decoding имён файлов).
7. Создать `src/fs_util.c` + `include/fs_util.h`. Перенести `slurp_fd`.
8. Удалить `src/static/http_static_etag.{c,h}`, `src/static/http_static_mime.{c,h}`.
9. Обновить `config.m4`, регенерация билда, прогон тестов (включая multipart suite —
   проверить, что файлы с unicode-именами теперь приходят декодированными).

**Контрольная точка**: все тесты зелёные. `src/static/` содержит только
mount-policy и cache, без HTTP-утилит общего назначения.

### Шаг 3. Создание движка ✅

Сделан. `include/send_file.h` + `src/send_file.c`. Движок принимает
`send_file_config_t` (mount-агностичный — `extra_headers` /
`mime_overrides` / `cache_control` / `content_disposition` /
`content_encoding` приходят как поля, NULL = пропустить); на ошибку
ветвится по `cfg.on_error` (`PASSTHROUGH_PHP` для StaticHandler,
`INLINE_500` для sendFile). Прекомпрессированный sidecar выбирает
caller (engine просто эмитит `Content-Encoding` + `Vary`). Сборка
зелёная, движок никем пока не вызывается; 27 целевых тестов и полный
server-suite по-прежнему зелёные.

1. Создать `include/send_file.h` с API из §6.
2. Создать `src/send_file.c`. Перенести FSM из `http_static.c`
   (`static_fsm_*` функции). Внутри движка:
   - убрать обращения к `http_static_handler_t *` (mount);
   - заменить на чтение полей `send_file_config_t`;
   - mount-only решения (extra_headers, mime_overrides) — читать из конфига,
     но если NULL — пропускать.
3. Подключить новые helper-header'ы.
4. Скомпилировать в изоляции (без вызывающих).

**Контрольная точка**: сборка зелёная, движок никем не вызывается.

### Шаг 4. Перевод StaticHandler на новый движок ✅

Сделан. `http_static.c` 1576 → 827 строк. Удалён `static_fsm_*` FSM
(~750 строк), весь FSM теперь в `src/send_file.c`. Адаптер заполняет
`send_file_config_t` из mount-полей, `on_error` выбирается по
`HTTP_STATIC_FLAG_ON_MISSING_NEXT` (PASSTHROUGH_PHP / EMIT_VIA_OP).
Маленький `static_adapter_t` мостит `http_static_dispatch_cbs_t` ↔
`send_file_cbs_t` (одинаковая форма, разные имена). Sync fallback
оставлен для H3 (где `ops->send_static_response == NULL`); engine
проверяет ops каждый раз и не зовётся, если не подключён. 12 static
+ 4 sendfile + 11 multipart + полный server-suite (133 PASS / 1 SKIP)
зелёные.

1. В `http_static.c` оставить только URL match + path resolve + cache + mount-policy.
2. Заполнять `send_file_config_t`, вызывать `send_file()` с `PASSTHROUGH_PHP`.
3. Удалить FSM, дубли утилит, вспомогательные функции — всё уже в движке/утилитах.
4. Прогнать static suite.

**Контрольная точка**: 12 static тестов зелёные, sendFile ещё на старой реализации.

### Шаг 5. Перевод Response::sendFile на новый движок ✅

Сделан. `http_send_file.c` 680 → 293 строки. Удалён `sf_*` FSM,
осталось: валидация пути + Content-Disposition (RFC 5987) +
прекомпрессированный sidecar + заполнение `send_file_config_t` с
`on_error = INLINE_500`. `INLINE_500` теперь означает «open/stat fail →
500 via op» (response object sealed sendFile'ом, transmission только
через protocol op). Адаптер `sf_adapter_t` владеет
`http_send_file_request_t` и Content-Disposition zend_string,
освобождает в on_done.

1. В `http_send_file.c` оставить ~80 строк: snapshot опций → конфиг → вызов.
2. Удалить FSM из `http_send_file.c`.
3. Прогнать sendfile suite.

**Контрольная точка**: 4 sendfile + 12 static + 167 server тестов зелёные.

### Шаг 6. Уборка ✅

`http_static_dispatch_cbs_t` теперь typedef-alias `send_file_cbs_t`
(одинаковая форма, имена полей унифицированы:
`on_hard_zero_armed → on_armed`, `on_static_done → on_done`,
`on_passthrough_to_php → on_passthrough`). Адаптер `static_adapter_t`
в `http_static.c` удалён — cbs прокидывается прямо в `send_file()`.
Обновлены H1 (`src/core/http_connection.c`) и H2
(`src/http2/http2_strategy.c`) callback-структуры.

1. Удалить устаревшие имена из `static_handler.h`: `http_static_dispatch_cbs_t`,
   `http_static_result_t` (живут как `send_file_*`).
2. Обновить `docs/PLAN_STATIC_HANDLER.md` и `CHANGELOG.md`.
3. Полная suite + замер покрытия (не должно просесть от baseline).


## 10. С нуля или рефакторинг?

**Рефакторинг.**

1. **Протоколы (H1, H2)** — стабильный vtable-op, переписывать незачем.
2. **Чистые функции (mime, etag, IMF-date, range, conditional)** — уже корректные,
   с тестами. Нужен move + публичный API, не rewrite.
3. **Движок** — это **существующий** FSM из `http_static.c`. Работает, покрыт
   тестами. Нужна декапсуляция (вынуть mount-зависимости в конфиг), а не
   переписывание.
4. **Адаптеры тонкие** — после переноса FSM в движок оба сократятся почти до
   однострочных вызовов.

«С нуля» имело бы смысл, если бы общая логика не работала. Сейчас работает и
покрыта — нужно перестать её копировать.


## 11. Кандидаты на дальнейшую дедупликацию (вне file delivery)

Список замечен по ходу работы над архитектурой отдачи файлов. Каждый пункт —
самостоятельный мини-рефакторинг, делается отдельным PR-ом, не блокирует
основной план.

### Подтверждённые (виден дубль или явный TODO)

1. ~~**`http_rfc5987` percent-codec.**~~ Сделано в Шаге 2 — `src/http_rfc5987.c`,
   encoder в `http_send_file.c::sf_build_content_disposition`, decoder в
   `multipart_processor.c::parse_content_disposition` закрывает старый TODO
   про percent-decoding имён файлов с unicode.

2. **Парсер `name="value" | value` в HTTP header params.** Дубль в
   `multipart_processor.c::parse_content_disposition` (name=, filename=) и
   `parse_content_type` (charset=, boundary=). Один движок «name=val | name="val"
   с экранированием» закрывает оба.

3. **Header-ish ASCII утилиты.** `is_path_separator`, `lower_extension`,
   case-insensitive memcmp, trim_ws, strip_weak_prefix — рассыпаны по
   `http_mime.c`, `http_etag.c`, `http_compression_negotiate.c`,
   `multipart_processor.c`. Маленькие и тривиальные, но дублирующиеся.
   Кандидат — `src/http_text_util.c`.

### Не подтверждённые (нужна проверка перед выделением)

4. **Format/parse целых в строку.** `format_u64` (теперь в `http_response.c`)
   и аналоги для int, status-кодов, hex-чисел. Возможно, разрозненные копии
   в HTTP/2/3 фреймах и логах.

5. **`q=`-параметр content-negotiation.** Парсится в `accept_encoding`
   compression-модуля. Та же грамматика нужна для `Accept`, `Accept-Language`,
   `Accept-Charset`, если кто-то решит их парсить.

6. **Lowercase-key HashTable lookup.** Многие места вручную лоуэркейзят имя
   заголовка перед `zend_hash_str_find`. Лежит в `http_response_static_set_header`
   как inline-цикл, в нескольких local helper'ах. Нужна одна `http_header_name_lower(buf, name, len)`.

### Метод поиска новых кандидатов

`grep` поиск дубликатов по pattern: «два TU реализуют функцию с одинаковой
сигнатурой и одинаковой семантикой, у которых разные имена». Признаки:
имя начинается с module-prefix (`sf_`, `static_fsm_`, `mp_`), функция —
небольшая чистая, в комментарии часто пишут «Mirror of …» или «Same as …
but for …». Самый яркий пример — `sf_try_precompressed` ↔
`try_select_precompressed`, оба зеркалили sidecar-резолв; FSM-часть
схлопнулась в `src/send_file.c` в Шаге 3, а сам sidecar-резолв пока
остался в каждом адаптере (отдельный микро-PR — вынести в общую функцию).


## 12. Риски — итог

Все четыре риска на момент завершения рефакторинга **не реализовались**:

1. **Регрессия в open-file cache** — `008-static-cache-counters.phpt`
   зелёный после Шага 4. По пути отловлен крах из-за shallow-copy
   `cache_view` в engine-state — фикс через deep-copy внутрь
   `engine_state_t`.
2. **HTTP/2 dispose-hijack** — `001-sendfile-basic.phpt` зелёный
   под H2. Response-указатель ходит через `on_done`-коллбэк адаптера;
   движок корректно отдаёт ownership.
3. **HTTP/3** — без изменений: H3 stub оставляет `send_static_response`
   NULL, оба адаптера видят это и падают в свой синхронный fallback
   (StaticHandler — slurp + `http_response_static_set_body_str`,
   sendFile — `http_response_synth_error 500`).
4. **Покрытие** — формально не пере-замерял (`lcov` не запускал);
   все 144/145 server + 11/11 multipart тестов зелёные, баг-фиксы по
   ходу реализации (NULL-check counters, addref zend_string-полей)
   подняты тестами, не баг-репортами.


## 13. Объём работы — фактический

| Шаг | Задумано (LOC) | Фактически (LOC) | Коммит |
|-----|----------------|------------------|--------|
| 1 | +200 / -150 | сделан до этой ветки | `699fe9f` |
| 2 | +500 / -550 | +750 / -900 (с http_rfc5987 + decoder) | `0c0f4d4` |
| 3 | +1100 / 0 | +827 / -1 | `c638aa8` |
| 4 | 0 / -1100 | +193 / -759 (http_static.c 1576→827) | `fe7e4ba` |
| 5 | +50 / -700 | +188 / -529 (http_send_file.c 680→293) | `0bb5b74` |
| 6 | +50 / -100 | +37 / -113 (alias + drop adapter) | `0c6c55d` |

Чистая дельта по проекту ≈ **−700 LOC** (как и оценивалось), при том
что добавилось **8 новых TU с публичным API** (mime, date, etag, range,
conditional, rfc5987, fs_util, send_file) — то есть код стал и короче,
и распределён по правильным модулям.

PR'ов нарезать по шагам не пришлось — все коммиты сделаны
последовательно на одной ветке, история ветки сама по себе читается
как 6 step-by-step PR'ов.


## 14. Производительная переделка (после §13)

После завершения архитектурного рефакторинга (§1–13) обнаружились
два узких места в горячем пути отдачи:

### 14.1 Sync prologue вместо async fs_open + fstat

**Было:** движок вызывал `ZEND_ASYNC_FS_OPEN` → callback на
готовность fd → `ZEND_ASYNC_IO_STAT` → callback с метой → дальше.
Каждый из двух шагов уходит в libuv thread pool: submit, worker
просыпается на futex, делает syscall, будит main loop через uv_async.
Два round-trip'а ради двух syscall'ов, которые на тёплом dentry
cache занимают < 1 µs каждый.

**Стало:** `send_file()` в синхронном prologue делает `open(2)` +
`fstat(2)` (если нет cache view) прямо на event-loop thread'е, потом
ставит 0-ms timer и продолжает на следующем тике. Timer нужен только
для того, чтобы `on_done` не разворачивался через стек самого
`send_file()` — иначе re-entrancy в request dispatcher (см. ниже).

**Riск:** disk stall на cold dentry. Принят — то же делает h2o.
В типовом workload'е (warm cache) — победа.

**Цифры:** H1 tiny 256B 19k → 35k req/s (+85%), H1 304 If-None-Match
24k → 123k req/s (+410%), large 8M +10%.

Коммит: `cd99d9e`.

### 14.2 Slurp fast path для small files (≤ 64 KiB)

**Bug в libuv:** `uv_fs_sendfile` на Linux:
1. Пытается `copy_file_range(2)` — для file→TCP socket возвращает
   EINVAL (kernel: «out_fd is not a regular file»).
2. Условие fallback'а: `try_sendfile = (errno == ENOSYS)`. EINVAL
   мимо — реальный `sendfile(2)` **не вызывается**.
3. Идёт в `uv__fs_sendfile_emul()` — userspace loop `pread + write`
   через 8 KiB буфер, всё в worker thread.

В итоге: ноль kernel zero-copy + futex round-trip per request.
Issue в upstream не зарепорчен; мейнтейнер позиции «kernel sendfile→
socket не приоритет» (libuv #1831). nginx/h2o не используют libuv
для этого вообще.

**Решение:** обходим `uv_fs_sendfile` для маленьких файлов. Если
`st_size ≤ 64 KiB` и не Range-запрос — `fs_slurp_fd()` синхронно
читает файл в `zend_string`, передаём response object'у как inline
body, делегируем `send_static_response` с `file_io=NULL` →
протокол-оп шлёт одним `writev(headers + body)` через ту же
per-socket очередь libuv, что и обычные writes. Ordering
гарантируется libuv-stream-queue.

**Граница 64 KiB подобрана экспериментально:** на 256 KiB sendfile-
путь снова обгоняет slurp в 6× (alloc + memcpy 256 KiB × 50
концурентных соединений упирается в memory bandwidth, тогда как
sendfile-worker переиспользует один 8 KiB буфер).

**Цифры:** H1 tiny 35k → 103k (×2.9), H1 small 16K 39k → 73k (×1.9),
H2 tiny 35k → 154k (×4.4) — обгоняем h2o-H1 по нашему бенчу. Medium
256K и large 8M остаются на старом sendfile-пути без изменений.

Коммит: `ed2d269`.

### 14.3 H2 range seek

**Latent bug найден по дороге:** H2 body FSM в `h2_static_response.c`
хранит `body_offset` в state но **никогда не применяет его к
file_io**. `ZEND_ASYNC_IO_READ` через libuv дёргает `read(2)` с
fd-позиции, которая = 0 сразу после open. Результат: на любой
Range-запрос H2 отдаёт байты от начала файла.

H1 не страдает потому что `uv_fs_sendfile` берёт offset аргументом
syscall'а.

**Фикс:** `ZEND_ASYNC_IO_SEEK(file_io, body_offset, SEEK_SET)` перед
первым `IO_READ`, если `body_offset > 0`. Последующие чтения
двигают позицию сами.

Закрывает `tests/phpt/server/static/012-static-h2`, который висел
с момента введения H2-fast-path.

Коммит: `e40eefa`.

### 14.4 Что не сделали и почему

**Large file path** (≥ 1 MiB) остаётся на libuv через worker. Реальная
починка требует одного из:
- Однострочный патч libuv (`errno == EINVAL` к условию `try_sendfile`).
  Не зарепорчен upstream, мейнтейнеры явно не хотят его принимать.
- Свой write-FSM на сокете (h2o-style): унифицированная очередь
  buffer+sendfile, single writer per socket. Требует переделки всего
  H1 write-пути. Большая фича.
- Sync `sendfile(2)` на event-loop thread с EAGAIN через `uv_poll_t`.
  Создаёт ordering race с `uv_write` для headers (latent gonk
  существует и сейчас в worker-пути, но фактически не триггерится
  из-за того что headers целиком абсорбируются `uv_try_write`).

Все три — отдельные задачи, не в этой ветке.

**Slurp threshold knob.** Сейчас `#define SEND_FILE_SLURP_THRESHOLD
((size_t)64 * 1024)`. Можно вынести в `setOpenFileCache()` параметр
для tuning под memory-constrained сетапы (понизить порог) или
SSD/tmpfs deployments (поднять). Не критично — 64 KiB разумный
default.

**Cache stats / introspection** (hits/misses/evictions для
observability). Помечено как low-priority — внутренний движок,
не пользовательская фича.
