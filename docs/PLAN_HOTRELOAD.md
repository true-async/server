<!-- Design doc for issue #93 (Hot reload). Synthesized via multi-agent workflow, 2026-07-01. All file:line citations verified against the tree at generation time; re-verify before implementation. -->

# Issue #93 — Hot-reload воркеров: дизайн

> **Решение (итог обсуждения 2026-07-01, пересмотрено).** Выбран подход **blue-green между потоками** через примитив **`ThreadPool::reload()` (channel-swap, см. A.3)** — своп task-канала + 1:1 замены по exit-токенам. Прежний вариант `respawn_worker`/`request_worker_exit` (индексный таргетинг, per-slot флаги) **ОТВЕРГНУТ**; его упоминания ниже по тексту — исторический анализ, не план. Хардening reload() (сериализация+схлопывание, identity-gate, единая точка токена) — `docs/PLAN_RELOAD_HARDENING.md`. Ниже — согласованный план и решения по деталям; дальше (раздел «Исходный анализ») идёт multi-agent-разбор, на котором это основано.

## A. Согласованное решение

### A.1. Подход
Воркеры — потоки одного процесса (общий opcache SHM, per-thread таблицы классов). Перезагрузить код в живом потоке нельзя (`Cannot redeclare` → fatal). Поэтому reload = **замена рабочего потока на свежий** (чистые таблицы → новый код), по одному/волной, при живом слушающем сокете. Это blue-green, только не между процессами/машинами, а между потоками.
- **Green** — свежий поток с новым кодом; поднимаем и проверяем, что взлетел.
- **Blue** — старые потоки; дают дожить текущим запросам и уходят.
- **Fail-safe:** green не поднялся → остаёмся на blue, простоя ноль (кривой деплой не роняет сервер).

### A.2. Алгоритм раскатки
1. Поднять **1 канарейку** (свежий поток). Не взлетела → отмена, ничего не тронули.
2. Взлетела → поднять **волну ~N%** свежих (N — ручка конфига, дефолт 50).
3. Соответствующие старые перевести в «дожатие» (A.4): снять с приёма новых, доработать в полёте.
4. Опустевшие старые погасить, поднять остаток свежих до целевого числа.
5. Итог: все потоки новые. Пик по памяти в момент волны ≈ ×1.5 — цена, регулируется N%.

### A.3. `ThreadPool::reload()` — примитив в движке (channel-swap, ПЕРЕСМОТРЕНО 2026-07-01)

> Прежний план (per-worker exit-флаг + `worker_ctx`-структура + индексные `respawn_worker`/`request_worker_exit`) **ОТВЕРГНУТ** как переусложнённый: индексный таргетинг не нужен, per-slot флаг даёт гонку «зомби» (старый и новый делят слот, сброс флага для нового разсигналивает старого). Итоговый дизайн — **свап канала**, ниже.

> **Дополнение 2026-07-02 (hardening, php-async d61b466):** описание ниже — базовая механика. Финальный примитив дополнительно: сериализует+схлопывает перекрывающиеся `reload()` (второй вызов ждёт и выполняется одной follow-up-ротацией всей когорты — поштучных reload нет); шлёт exit-токен на ВСЕХ путях выхода воркера (после `zend_end_try`, включая bailout); гейтит токен по identity (`wc->channel == reload_old` — умершая замена/стрэгглер не в счёте); `task_channel` — атомик. Полный дизайн и 13 прогнанных сценариев — `docs/PLAN_RELOAD_HARDENING.md`.

**Идея.** Воркер захватывает `channel = pool->task_channel` **один раз на старте**, поэтому когорты сами привязаны к своим каналам. reload = дать новым воркерам **другой** канал и закрыть старый:
```
reload():                          // выполняется на корутине (может await'ить)
  new = async_thread_channel_create()
  pool->task_channel = new         // новые submit'ы и будущие воркеры — на new
  pool->reload_notify = notify_ch  // канал, куда уходящие старые шлют "я вышел"
  close(old)                       // generic: old-воркеры проснулись в receive → false → выходят
                                   // server: старый self-stop'нулся → start() вернулся → receive(old)=false → выход
  repeat N раз:                    // rolling, БЕЗ 2N
    spawn 1 свежего на new         // boot = новый код (чистые CG/EG)
    await receive(notify_ch)       // ждём, что один старый доумер
  cleanup
```
Завершение старого — **штатный механизм**: `receive()` на закрытом канале вернул false. Уходящий старый воркер (у кого `captured_channel != pool->task_channel`) перед выходом шлёт токен в `reload_notify`; reload-корутина ловит и поднимает замену **1:1** → в любой момент ~**N** воркеров, overlap ограничен (пик +1), никакого 2N.

**Что это убирает:** индексы, exit-флаг, структуру `worker_ctx`, два vtable-метода. **В ABI — один метод** `reload(pool)` вместо двух.

**Свойства:**
- targeting не нужен (когорты разведены каналами);
- пробуждение idle-старых — тем же `close(old)`;
- раздача задач не меняется (новые тянут из new — та же MPMC-балансировка);
- нет epoch/протухших команд (новые физически на другом канале);
- свежий **pthread** (чистые таблицы → `bootloader_snapshot` перевыполняется → новый код через re-`require`+`opcache_invalidate`).

**API:** `Async\ThreadPool::reload(): void` (или `Future`). Низкоуровневых `respawnWorker`/`requestWorkerExit` в PHP НЕТ — только `reload()`. Серверный `HttpServer::reload()` строится поверх (плюс дренаж `#74`/GOAWAY + self-stop старых серверных воркеров).

### A.4. Дренаж по протоколам (старый поток «уходит с приёма»)
- **TCP:** **закрыть свой слушающий сокет** (не сокеты соединений!). Под REUSEPORT именно закрыть → ядро раздаёт новых остальным. Отдельное состояние `reloading`, НЕ переиспользовать CoDel-паузу (иначе авто-resume, `http_server_class.c:1277`).
- **HTTP/1.1:** на следующий ответ `Connection: close`.
- **HTTP/2 / HTTP/3:** **GOAWAY** — уже реализовано (`http2_strategy.c:1048`; `http3_dispatch.c:614` `nghttp3_conn_shutdown`), висит на общей **drain-эпохе** (`http_server_drain_evaluate`). Reload = дёрнуть эпоху потока.
- **WebSocket:** `1001 Going Away`.
- Недоделанное дожать через `#74` (`http_server_class.c:1620`) с таймаутом `gracefulMs`, хвост — cancel.

### A.5. HTTP/3 — два режима (нюансы, не блокеры)
- **Реактор-режим** (`TRUE_ASYNC_SERVER_REACTOR_POOL=1`, opt-in): реактор — отдельный C-поток (сокет+QUIC+CID-стиринг), **без PHP**. Reload воркеров его НЕ трогает → соединения переживают прозрачно, следующий запрос уходит на свежий воркер, **GOAWAY не нужен**. Нюанс — хендофф: `worker_registry` нужен **disable слота** (сейчас только publish), `worker_inbox` — **drain-mode** + освобождение на своём потоке ПОСЛЕ того как реактор перестал слать (иначе UAF), старый доедает уже поставленные в ящик запросы (иначе QUIC-стрим повиснет без ответа).
- **Дефолт-режим** (воркер сам владеет UDP+QUIC): у UDP нет отдельных «слушающий»/«соединения» — всё на одном сокете, закрыть = убить живые соединения. Дренаж только на уровне QUIC — **GOAWAY-эпоха** (A.4). Краткое окно: часть новых Initial по REUSEPORT-хешу попадёт на закрывающийся сокет → отдать/отклонить, клиент переоткроется. Чистая H3-перекатка — реактор-режим.

### A.6. Какие файлы отслеживать
Мейнстрим-паттерн (Octane/Hyperf/Air совпадают): **конфиг-allowlist каталогов + расширений + ignore-глоб**, не «всё подряд». Три режима:
- **Явные пути (дефолт):** список каталогов + `extensions` (дефолт `['php']`) + `ignore` (дефолт `vendor/`, `var/cache/`, `.git/`, `*.log`). Предсказуемо.
- **Included-files (как Swoole/mezzio):** следить ровно за `get_included_files()`. Точно, но не видит ещё-не-подключённые файлы.
- **Sentinel-файл (prod):** один файл, деплой его `touch`'ает атомарно → reload. Развязывает «что изменилось» и «когда перезагружать».
- **Драйвер:** только событийный, **без сканирования**. Раньше блокером было, что `Async\FileSystemWatcher recursive:true` на Linux/inotify молча не работал (inotify не рекурсивен). **ИСПРАВЛЕНО** в php-async (ветка `fs-watcher-recursive-linux`): при `recursive:true` на не-нативных платформах вотчер сам обходит дерево и вешает по inotify-watch на каждый подкаталог, динамически добавляя watch при создании новых папок; наружу — один объект. Тест `tests/fs_watcher/011`, весь сьют 10/10. → POLL/`pollIntervalMs` из API убираем.
- **Debounce — РЕАЛИЗОВАН ВНУТРИ вотчера** (та же ветка, `tests/fs_watcher/013`+`014`, сьют 13/13). Новые арги `FileSystemWatcher`: `debounceMs`(0=выкл), `maxHoldMs`(0=без потолка — ломает бесконечный шторм: flush ≤ maxHold после первого изменения), `extensions`(`['php']`=фильтр по расширению). Механика reactor-only без корутины: fs-событие→фильтр→`dirty`+rearm HIDDEN+MULTISHOT таймера тишины; сработал→одно схлопнутое событие `filename=null`+фаяр trigger-события; итератор в debounce-режиме ждёт на нём, а не на fs-event → спит сквозь всплеск. HIDDEN не влияет на детектор дедлоков. Поэтому debounce/фильтр/maxHold в userland-обёртке дублировать НЕ нужно — это в рантайме.

### A.7. API (следует конвенциям проекта: setter-not-INI, enum UPPER_CASE)
```php
enum ReloadStrategy { case RESPAWN; case SOFT; }     // дефолт RESPAWN

final class HotReloadConfig {
    // ЧТО следить (dev) — событийно через Async\FileSystemWatcher(recursive:true)
    public function watch(string ...$paths): static;            // каталоги/файлы
    public function extensions(string ...$ext): static;         // дефолт ['php']
    public function ignore(string ...$globs): static;           // vendor/*, var/cache/*
    public function watchIncludedFiles(bool $on = true): static;
    // КАК триггерить (prod)
    public function triggerFile(?string $sentinel): static;
    public function onSignal(?\Async\Signal $sig): static;      // SIGHUP
    // КАК катить
    public function strategy(ReloadStrategy $s): static;        // дефолт RESPAWN
    public function debounceMs(int $ms): static;                // дефолт 300
    public function gracefulMs(int $ms): static;                // таймаут дренажа/воркер
    public function batchPercent(int $pct): static;             // размер волны, дефолт 50
    public function onBeforeReload(?\Closure $cb): static;      // как Swoole BeforeReload
    public function onAfterReload(?\Closure $cb): static;       // AfterReload
    // + геттеры-зеркала
}
$config->setHotReload(?HotReloadConfig $c): static;

// программно / низкоуровнево
HttpServer::reload(): Future;                    // ручной/admin триггер
Async\ThreadPool::respawnWorker(int $i): Future; // примитив
```
Секьюр-дефолты: hot-reload OFF по умолчанию; watch-режим — dev-only (prod через triggerFile/signal); ignore всегда включает vendor/.git/cache/log (не устраивать reload-шторм). debounce обязателен ВНЕ приёма событий (иначе N файлов = N reload'ов).

### A.8. opcache
Только `opcache_invalidate($file, true)` изменённых файлов до респавна. **Никогда `opcache_reset()`** (в prod `validate_timestamps=0`; под ZTS reset потенциально небезопасен — см. §7 ниже).

### A.9. Фазировка / PR
- **Фаза 1 (php-async):** A.3 — `ThreadPool::reload()` (channel-swap: своп канала + 1:1 замены по exit-токенам) + ABI v0.22. **СДЕЛАНО** (ветка fs-watcher-recursive-linux, df5dbfa + hardening по PLAN_RELOAD_HARDENING.md).
- **Фаза 2 (сервер, TCP):** A.2 + A.4(TCP/h1/h2/WS) + A.6/A.7(watch+API) + A.8 → `HttpServer::reload()` + blue-green. Закрывает #93 для HTTP/1.1 и HTTP/2. **Обязательно логирование через СОБСТВЕННЫЕ обработчики сервера** (`http_logf_info` на `server->log_state`; сейчас пишутся только `server.start`:3338 / `server.stop`:3459): (1) `pool_worker_handler` (:1809) логирует `worker.start` / `worker.exit` — при ротации старая и новая когорты сами видны в логе без доп. машинерии; (2) reload-путь оркестратора логирует `reload.start` (+триггер: watcher/SIGHUP/ручной), `reload.done` (длительность) и деградации (замена не поднялась).
- **Фаза 3 (H3 дефолт):** A.5 дефолт-режим — GOAWAY-дренаж.
- **Фаза 4 (H3 реактор):** A.5 реактор-режим — `worker_registry` disable + `worker_inbox` drain-mode.

Референсы: Swoole `reload()`/`max_wait_time` (Приложение A), Laravel Octane `watch`-config + chokidar/polling, Hyperf `watcher` (dir/file/ext/exclude_dir/scan_interval, ScanFileDriver), Air `.air.toml` (include_ext/exclude_dir).

---

# Исходный анализ (multi-agent synthesis)

Статус: internal design doc. Все ключевые утверждения сверены с кодом (`file:line` в тексте). Вывод короткий: **«honest» hot-reload в этой модели распадается на два принципиально разных механизма**, и большинство наивных формулировок #93 («перезапусти bootloader — хендлеры перерегистрируются») в live-потоке физически небезопасны.

---

## 1. Ключевой вывод про модель потоков (читать первым)

**Воркеры — это OS-потоки внутри ОДНОГО процесса, а не дочерние процессы.** Пул создаётся через `ZEND_ASYNC_NEW_THREAD_POOL_EX` (`src/http_server_class.c:2526-2529`); каждый воркер — `pthread`, занятый ровно одной долгоживущей задачей `pool_worker_handler`, которая вызывает `$server->start()` (`src/http_server_class.c:1819`) и суспендится в accept-цикле на весь срок жизни потока. Никакого `fork` нет.

Из этого — три жёстких следствия, которые определяют весь дизайн:

1. **opcache SHM — process-global и общий для всех воркеров.** Одна карта на процесс. А вот `CG(class_table)` / `EG(function_table)` — **per-thread** (создаются `ts_resource(0)` на старте потока, заполняются из общего образа). То есть символьные таблицы у каждого воркера свои, но байткод-кэш один.

2. **В live-потоке нельзя переобъявить класс/функцию.** Bootloader — это `require` app-файлов. Он выполняется **ровно один раз** на воркер, до receive-цикла (`php-src/ext/async/thread_pool.c:186-257`). Повторный `require` в уже живом потоке ударит в `Cannot redeclare class/function` — это **compile-time fatal (E_COMPILE_ERROR → `zend_bailout`)**, а не ловимый `Throwable`. То есть «перезапустить bootloader, чтобы подхватить новый код» = гарантированный краш воркера. Это тот же класс отказа, что и починенный баг «setWorkers ломает h2/TLS».

3. **Даже если переобъявление обойти — хендлеры не перерегистрируются сами.** Closure-хендлеры копируются в воркер **ровно один раз** при спавне (transfer_obj LOAD, `src/http_server_class.c:4371-4437`), в собственный `protocol_handlers` воркера. Bootloader вызывается **с нулём аргументов** (`src/http_server_class.c:2513-2521`, `zend_fcall_info_init(..., 0, ...)`) — у него **нет ссылки** на worker-clone `$server`, поэтому он в принципе не может тронуть `protocol_handlers` живого воркера. Re-run bootloader'а перевыполняет ЕГО собственный op_array, но зарегистрированный хендлер — это отдельный Closure-объект, чей `fci_cache.function_handler` по-прежнему указывает на исходно скомпилированный код.

**Скорректированная ментальная модель.** Существуют ровно два когерентных примитива перезагрузки, и их нельзя путать:

- **SOFT-reload (состояние/роутинг/конфиг/closure-графы):** работает в живом потоке СЕГОДНЯ, ноль C-изменений — но только для кода, оформленного как анонимные op_array'и / `return`-модули. Именованные классы/функции — не перезагружаемы.
- **HARD-reload (изменение тела именованного класса/функции):** требует **свежего потока** с чистыми `CG/EG`, который перекомпилирует обновлённый исходник. В живом потоке это невозможно в принципе.

Всё остальное в документе — это как аккуратно упаковать эти два примитива, не наступив на грабли, которые критики нашли во всех трёх черновых дизайнах.

---

## 2. Рекомендуемый дизайн — двухуровневый гибрид

Ни один из трёх черновиков не годится «как есть»:

- **Design 1 (stable-shim + return-registry)** — единственный, что реально работает в живом потоке (судья эмпирически подтвердил: `opcache_invalidate($f,true)` + `require` `return new class{}` перекомпилирует, новый код виден, старый инстанс доигрывает старый код, `Cannot redeclare` нет). Но он **переопределяет #93**, а не выполняет: именованные классы не перезагружает, WS/SSE молча крутят старый код, и есть unbounded per-thread рост (по одной анонимной class-table записи на reload).
- **Design 2 (in-place drain-swap epoch)** — **отклоняется целиком**. Критики сломали его насмерть: `#74 cancel()` терминален (нет `CLR_CANCELLED`), keep-alive-запрос гоняется в уже отменённый scope, `on_connection_close` авто-возобновляет листенеры посреди reload'а (`src/http_server_class.c:1277-1280`), `should_shed_request` продолжает кормить дренируемый scope keep-alive-запросами (`src/http_server_class.c:940-955`), нет списка соединений чтобы перецелить `conn->scope` (`src/http_server_class.c:967-972`), SSE нельзя «pre-parent» в durable-scope. Слишком много острых краёв ради выигрыша, которого нет над blue-green.
- **Design 3 (blue-green respawn потока)** — **архитектурно верный** ответ на HARD-reload, но заблокирован на неготовом ABI (нет `respawn_worker`, нет cross-thread wake) и содержит фактическую ошибку в zero-downtime (REUSEPORT-пауза не перемаршрутизирует TCP).

**Рекомендация — три tier'а, ранжированные по готовности:**

### Tier A — SOFT-reload, in-process, чистый userland (MVP, отгружается сейчас)
Это Design 1, обёрнутый в серверную эргономику. C-стабильный тонкий делегат-хендлер, который **никогда не свопается**, читает per-thread слот каждый запрос; reload перекомпилирует `return`-модули (после `opcache_invalidate`) и атомарно свопает слот. **Никакого drain, никакой паузы листенеров, никакого teardown потока.** Покрывает 90% dev-кейса (роуты, wiring, конфиг, closure-хендлеры) и config/routing-триггеры из #93. Честный потолок: изменение тела `class UserController` не подхватится — для этого Tier B/C.

### Tier B — HARD-reload через process-level blue-green (работает сегодня, ноль нового server-ABI)
Классический ответ для many-thread single-process: **два процесса за одним SO_REUSEPORT-портом**. Сервер уже биндит REUSEPORT (`src/http_server_class.c:3074-3075`), поэтому оркестратор (systemd/k8s/скрипт) поднимает новый процесс, оба сокета в группе, старый процесс дренирует (`#74` на своём teardown-пути отрабатывает штатно) и выходит. Это **обходит ВСЕ внутрипроцессные блокеры** (`respawn_worker`, `#11`, transfer-guard) и даёт настоящую перезагрузку именованных классов с чистыми `CG/EG` в новом процессе. Для named-class-изменений это **рекомендуемый способ до появления Tier C**.

### Tier C — HARD-reload, in-process per-worker rolling respawn (полноценный #93, отложен на ABI)
Это Design 3 с фиксами критиков. Настоящая «rolling per-worker reload» внутри одного процесса. **Заблокирован** на новом php-async ThreadPool ABI (`respawn_worker` + per-worker poison-pill), cross-thread parent→worker wake (`#11`/`#72` groundwork) и снятии transfer-while-running guard. XL-объём. Пока ABI нет — Tier C не строим; его роль закрывает Tier B на уровне процессов.

**Почему именно так.** Судья поставил Design 1 highest (51) именно за «единственный, что работает сегодня» — это Tier A. Судья по Design 3 (45) прямо написал: «per-worker rolling needs substantial new ABI first… freshest safely-reloadable unit today is the whole process» — это ровно граница Tier B/Tier C. Design 2 (37) забракован критиками по существу. Двухуровневая упаковка отгружает пользу немедленно (Tier A + Tier B), а самый дорогой и заблокированный кусок (Tier C) изолирует за явной ABI-зависимостью.

---

## 3. Последовательность reload одного воркера

### 3A. SOFT-reload (Tier A) — то, что выполняется в живом потоке

Отображение на существующую машинерию (переиспользуется как есть, ничего не дренируется):

| # | Шаг | Механизм / цитата |
|---|-----|-------------------|
| 1 | Триггер-корутина в **top-level scope воркера** наблюдает изменение | `Async\FileSystemWatcher` (`php-src/ext/async/fs_watcher.stub.php:13-59`) / `Async\signal(SIGHUP)` (`php-src/ext/async/async.stub.php:262-286`) |
| 2 | Debounce/coalesce (см. риск ниже — **НЕ через `Async\delay` в теле foreach**) | userland |
| 3 | Для каждого изменённого файла: `opcache_invalidate($f, true)` — **никогда `opcache_reset()`** | `ZendAccelerator.c:1396-1421` (пометка corrupted, память→wasted, без free) |
| 4 | Пересборка графа в ЛОКАЛ: `$new = ($factory)()` — re-`require` `return`-модулей → свежие анонимные op_array'и | подтверждено эмпирически судьёй; `CG(rtd_key_counter)++` даёт уникальное имя каждый compile |
| 5 | Fail-safe: весь build в `try/catch (\Throwable)`; на ошибке — оставить старый граф | userland (**ограничение**: `Cannot redeclare` = bailout, НЕ `Throwable` — не ловится) |
| 6 | Атомарный своп: `Reloader::$app = $new` (одно присваивание, не точка suspend) | per-thread class-static в TSRM-копии `EG` |
| 7 | Старый граф освобождается по refcount, когда последний in-flight запрос, захвативший его, завершится | GC |

Ключ: **C-хендлер никогда не свопается.** Регистрируется стабильный делегат `addHttpHandler(fn($req,$res) => Reloader::current()->handle($req,$res))`. Диспетчер читает живой fcall из `protocol_handlers` через кэшированный `fci_cache` (`src/core/worker_dispatch.c:70-102`) — этот fcall перманентен и корректен; меняется только делегируемая цель в слоте. Никакого `#74`, никакого `conn->scope`-staleness, никакого one-shot-guard.

### 3B. HARD-reload одного воркера (Tier C) — целевая последовательность (после ABI)

| # | Шаг | Механизм / цитата | Готовность |
|---|-----|-------------------|-----------|
| 0 | (раз на reload, до слотов) `opcache_invalidate($f,true)` каждого изменённого файла | `ZendAccelerator.c:1396-1421`; SHM process-global → взводит перекомпиляцию для любого свежего compile | есть |
| 1 | Координатор в **pool-parent top-level scope** выбирает слот `i` (последовательный ролл) | — | нужен координатор |
| 2 | Parent будит reload-driver воркера `i` cross-thread | **НЕТ ABI** — это `#11`/`#72` (channel send/receive суспендят, poll_cb не корутина) | **блокер** |
| 3 | Квиесценция входящего: **не `pause_listeners`** (см. §4) — передать listener-fd свежему воркеру / полагаться на N-1 пиров | `pause_listeners` только `le->base.stop` (`src/http_server_class.c:741`) — НЕ убирает сокет из REUSEPORT-группы | нужна fd-передача |
| 4 | Шед долгоживущих потоков (WS `1001 Going Away`, финал SSE) **до** cancel | нужен **per-worker реестр соединений** — сегодня его НЕТ (`src/http_server_class.c:967-972`) | **нужен реестр** |
| 5 | `#74` drain по server_scope воркера: Phase1 `awaitCompletion(grace)`, Phase2 `cancel()+awaitAfterCancellation()` | `src/http_server_class.c:1620-1666` — работает как есть на teardown-пути умирающего потока | есть |
| 6 | Воркер `i` нотифицирует свой `wait_event` → `start()` возвращается → handler возвращается | `src/http_server_class.c:3367/3376` (suspend), teardown `3465` | есть |
| 7 | Parent детектит завершение слота и вызывает `respawn_worker(pool, i)`: join старого handle, `thread_pool_start_worker` свежего pthread → чистые `CG/EG` → bootloader перекомпилирует новый исходник | **НЕТ ABI**: vtable = `{close, dispose, submit_internal}` (`Zend/zend_async_API.h:2007-2011`), one-time create-loop, `close()` рушит ОБЩИЙ channel для всех | **блокер** |
| 8 | Parent `submit_internal` свежий clone сервера в новый воркер | **упирается в guard** `Cannot transfer HttpServer while running` (`src/http_server_class.c:4246-4250`); `src->running==true` весь срок пула | **нужно снять guard / альт-доставка** |
| 9 | Свежий воркер `start()`: минтит НОВЫЙ server_scope, регистрирует хендлеры через **worker-init hook** против свежескомпилированных closure, поднимает polling на listener-fd | server_scope: `src/http_server_class.c:2925/2938`; hook закрывает «bootloader без server ref» | нужен hook |
| 10 | Parent ждёт «worker i ready», переходит к `i+1` | — | нужен ready-сигнал + timeout |

Пункты 2, 7, 8 — жёсткие блокеры (см. §11). Именно поэтому Tier C отложен, а named-class-перезагрузку сегодня закрывает Tier B (§2).

---

## 4. Rolling по пулу, zero-downtime, непрерывность листенера

**Главная поправка к черновикам (критики сломали это в Design 2 и Design 3):** `http_server_pause_listeners` вызывает **только** `le->base.stop(&le->base)` (`src/http_server_class.c:741`) — это останавливает *поллинг* fd, но **не закрывает сокет и не выводит его из SO_REUSEPORT-группы**. На Linux TCP ядро выбирает сокет группы по хешу 4-кортежа в момент SYN, независимо от того, зовёт ли userspace `accept()`. Комментарий в коде «REUSEPORT hash excludes this worker» (`src/http_server_class.c:725`) — **фактически неверен** для TCP: ~1/N новых соединений будут копиться в backlog приостановленного сокета весь drain-window, а не уходить на пиров. На это прямо указывает и grounding-факт («paused SYNs queue in kernel backlog»).

**Корректный механизм непрерывности листенера — эксплуатировать то, что это ОДИН процесс.** Файловые дескрипторы листенеров — process-global; таблица fd общая для всех потоков. Отсюда две honest-опции:

- **(Рекомендуется для Tier C) Parent-owned listener fds + shared polling.** Parent биндит листенеры один раз до спавна воркеров; воркеры поллят общие parent-owned fd. Респавн воркера тогда **вообще не трогает fd** — parent-сокет остаётся забинденным, любой живой воркер (включая свежий) принимает с него. Это «shared-fd» модель, и она gap-free. Требует отойти от текущей «каждый воркер — свой REUSEPORT dup».
- **(Если оставляем per-worker REUSEPORT-сокеты) fd-handoff.** Свежий воркер **адаптирует тот же listener-fd**, который держал уходящий (fd валиден через смену потока в одном процессе), а не биндит новый. Никакой churn REUSEPORT-группы.

**Zero-downtime аргумент (честный).** Для `N≥2`: пока слот `i` дренирует, остальные `N-1` воркеров принимают. Доля новых соединений, приходящаяся на fd слота `i`, в окне «старый перестал поллить — новый ещё не начал» **очередится в backlog этого fd** (не дропается, если backlog не переполнен и drain ограничен по времени). Чтобы минимизировать блип: (а) не ждать полный grace, если scope уже пуст (`http_server_scope_is_finished`, `src/http_server_class.c:1639`); (б) перекрыть — поднять polling свежего воркера на общем fd до финализации старого. Для `N=1` gap неизбежен без parent-fallback-сокета в группе — rolling честно zero-downtime только при `N≥2`.

**НЕ полагаться на авто-hysteresis.** `http_server_on_connection_close` авто-возобновляет листенеры при `listeners_paused && active_connections <= pause_low` (`src/http_server_class.c:1277-1280`). `listeners_paused` — один bool без «reason». Если reload использует ту же паузу, drain-закрытия соединений просядут `active_connections` ниже `pause_low` и **авто-возобновят приём посреди reload'а**. Поэтому maintenance-пауза Tier C должна быть отдельным состоянием (`server->reloading`), не переиспользующим CoDel/hard-cap-паузу. (В Tier A проблемы нет — там листенеры не трогаются вообще.)

**Взаимодействие с `setWorkers()`.** `setWorkers(N)` создаёт fixed-size пул через `NEW_THREAD_POOL_EX((int32_t)workers, ...)` (`src/http_server_class.c:2527`). Rolling Tier C итерирует по этим `N` слотам с staggered-задержкой `worker_index * reload_stagger_ms`, чтобы никогда не паузить весь пул одновременно. Но: staggered timing без interlock'а — приблизительный ролл (нет cross-thread барьера из-за `#72`); если `stagger < drain_duration`, несколько воркеров паузятся одновременно → просадка ёмкости. Это открытый риск (§12).

---

## 5. Триггеры

**Где живёт триггер-корутина (критично).** Триггер обязан быть в **top-level scope** (внутри `setBootloader()`-closure либо через `Async\spawn()` до `$server->start()`), **НЕ в server_scope**. server_scope создаётся как child текущего scope (`src/http_server_class.c:2925`), и `#74` Phase2 `cancel()` каскадно убьёт всё в его поддереве — включая reload-драйвер, если тот там окажется. `start()` суспендится на `wait_event` (`src/http_server_class.c:3367/3376`) и не возвращается, поэтому `spawn()` нельзя делать *после* `start()` в той же корутине — только до, либо из bootloader'а.

**DEV — FileSystemWatcher.** Естественно-персистентный (`foreach` до `close()`), re-arm не нужен:

```php
// внутри setBootloader() closure — top-level scope, раз на воркер
Async\spawn(static function () use ($appDir) {
    // ВНИМАНИЕ: recursive:true на Linux/inotify libuv НЕ реализует —
    // флаг молча игнорируется, видны только прямые дети каталога.
    // Watch'им плоский каталог или sentinel-файл, а не всё дерево.
    foreach (new Async\FileSystemWatcher($appDir /*, recursive:false */) as $ev) {
        Reloader::scheduleReload($ev->path); // debounce внутри, НЕ Async\delay в foreach
    }
});
```

**PROD — SIGHUP, с обязательным re-arm.** `Async\signal` — **one-shot**: после резолва зовёт `signal_event->stop()` (`php-src/ext/async/async.c:1322`, `Signal::SIGHUP=1`). Цикл обязан перевзводить сигнал каждую итерацию:

```php
Async\spawn(static function () {
    while (true) {
        Async\await(Async\signal(Async\Signal::SIGHUP));
        Reloader::reload();
    }
});
```

**Грабли SIGHUP (критик подтвердил):** между резолвом и следующим `Async\signal()` есть окно без хендла; libuv возвращает дефолтную диспозицию, а дефолт SIGHUP = **Terminate**. Так как все воркеры в одном процессе, SIGHUP, пришедший в это окно (ровно когда все проснулись от первого HUP и крутят `reload()`), **убьёт процесс целиком**. Плюс, одиночный process-SIGHUP может разбудить watcher лишь одного произвольного потока. **Вывод:** для prod fan-out надёжнее **filesystem-as-broadcast** — deploy трогает sentinel-файл, каждый воркер его watch'ит и сам себя перезагружает; это обходит и `#72` (нет cross-thread канала), и SIGHUP-фрагильность. SIGHUP — опционально, с явной оговоркой про окно.

**ADMIN/programmatic.** `HttpServer::reload(array $changedFiles = [])` из top-level-корутины admin-эндпоинта. Одиночный admin-хит попадает в один воркер → его хендлер сам трогает sentinel для fan-out на остальных.

---

## 6. WebSocket / SSE

**Как они сидят в scope.** WS-корутина спавнится в `conn->scope` (`src/websocket/ws_dispatch.c:191-192`), а `conn->scope == server_scope` (`src/core/http_connection.c:3092`). SSE — это **не отдельная корутина**, а флаг режима ответа `response->sse_mode` (`src/http_response.c:883`), обслуживаемый внутри той же request-handler-корутины. Оба — в поддереве server_scope.

**Политика по tier'ам:**

- **Tier A (soft):** потоки **не трогаются** — это и сила, и честное ограничение. Нет `#74` cancel → нет generic Scope-cancelled исключения, нет `1001`. WS/SSE **продолжают крутить СТАРЫЙ код** до естественного закрытия. Чтобы согнать на новый код — app-level opt-in: цикл WS/SSE периодически сверяет `Reloader::version()` со своей стартовой версией и при дрейфе добровольно шлёт `1001` (WS) / `retry`+end (SSE). Автоматического handoff нет.

- **Tier C (hard, respawn):** поток умирает, поэтому потоки **обязаны быть согнаны до Phase2 cancel** — иначе `cancel()` даёт им резкий TCP-close без `1001` (server_scope имеет `DISPOSE_SAFELY` снятым, `src/http_server_class.c:2938`). Нужен **явный shed-hook**, который перечисляет живые WS-conn'ы и ставит им `1001 Going Away`, и завершает активные SSE-ответы, **до** drain. Тонкость, которую критики подчёркивали: enqueue `1001`-фрейма **не разматывает** корутину, припаркованную в `foreach recv()` — надо ещё закрыть read-сторону/отменить стрим, иначе он всё равно словит Phase2 hard-cancel. И для перечисления нужен **per-worker реестр соединений, которого сегодня нет** (`src/http_server_class.c:967-972` — «no server-side list of connections»). Это отдельная C-задача (§10/§11).

- **`durable_scope` (идея из Design 2, взята как secondary):** держать WS/SSE в **отдельном, никогда-не-отменяемом** scope, а не в per-request epoch. Тогда drain не трогает потоки (code-version pinning — штатная семантика hot-reload для персистентных соединений). Работает для WS (отдельный спавн можно направить в другой scope), но **не для SSE** — SSE-ность неизвестна до выполнения хендлера, «pre-parent» нечего. Поэтому durable_scope — опциональное улучшение для WS, не универсальное решение.

**Рекомендация:** Tier A — версионный opt-in (документировать как ограничение). Tier C — обязательный shed-hook `1001` + `Response::onGoingAway(Closure)` для кастома; durable_scope для WS — опционально.

---

## 7. opcache

**Рекомендация однозначна: только per-file `opcache_invalidate($file, true)`, НИКОГДА `opcache_reset()`.** Верно для всех tier'ов.

**Почему не `opcache_reset()` (сверено):**
- В персистентном потоке `opcache_reset()` **не освобождает SHM inline** — он лишь планирует рестарт (`restart_pending=true`) и немедленно **выключает акселератор** (`accelerator_enabled=false`) под SHM-локом (`zend_accelerator_module.c:905-925` → `ZendAccelerator.c:3545-3578`).
- Реальный wipe SHM отложен в `ZEND_RINIT_FUNCTION(zend_accelerator)` (`ZendAccelerator.c:2666`, блок `2731-2760`), гейтед `accel_is_inactive()`. В async-runtime PHP RINIT **не повторяется на каждый HTTP-запрос**, поэтому mid-run reset просто **выключает кэш до конца жизни процесса**, а не перезагружает его.
- Хуже: под single-process ZTS гейт неактивности использует per-process `fcntl F_GETLK` (`ZendAccelerator.c:342-360`, `920-945`), который **не видит sibling-потоки того же процесса**. `F_GETLK` не сообщает о собственных read-локах процесса → из соседнего потока `accel_is_inactive()` может ложно вернуть true и **wipe SHM под живым op_array другого воркера** → UAF/SIGSEGV. Запись сериализуется `zend_shared_alloc_lock` (`zts_lock` + `fcntl F_SETLKW`, `zend_shared_alloc.c:496-508`), но это не спасает от ложного inactive-гейта.

**Почему `opcache_invalidate($f,true)` безопасен:** помечает `persistent_script->corrupted` (timestamp=0, память→wasted) под write-локом, **не освобождая живой op_array** (`ZendAccelerator.c:1396-1421`). UAF-safe; следующий compile файла перекомпилирует из исходника. SHM process-global → один invalidate «взводит» перекомпиляцию для всех воркеров (но каждый воркер всё равно должен сам re-`require`, чтобы собрать свой thread-local граф).

**`validate_timestamps`:** php-release дефолт = **On** (авто re-stat + перекомпиляция для любого свежего compile — dev). Но HttpArena prod-Docker ставит `validate_timestamps=0` (`examples/docker/Dockerfile:39`) → opcache **никогда** не перечитывает файлы → в prod invalidate **обязателен**, иначе устаревший байткод обслуживается вечно.

**Долгий риск (критик, survives):** каждый invalidate добавляет размер сброшенного скрипта в `wasted_shared_memory`, каждая перекомпиляция кладёт свежую копию. При частых reload'ах wasted растёт монотонно; на пороге `max_wasted_percentage` opcache **сам планирует рестарт** — тот самый reset-wipe, что мы запрещаем, только недетерминированно. Смягчение: per-file invalidate только реально изменённых файлов + периодический full-restart процесса (в prod — Tier B blue-green). Для Tier A это ставит потолок на число reload'ов до неизбежного рестарта процесса.

---

## 8. reactor-pool mode (#80)

В reactor-pool режиме воркер-клон **не владеет листенером** — он `continue`'ит мимо udp_h3-listen и лишь публикует `worker_inbox` в `worker_registry` (`http_server_worker_inbox_up`, `src/http_server_class.c:2950`). C-реактор владеет листенером и роутит распарсенные запросы в mailbox воркера.

**Следствия для дизайна:**

- **Tier A (soft) — ортогонален.** Листенер не паузим, scope не дренируем: диспетч → стабильный тонкий хендлер → чтение per-thread слота → делегат. Своп слота идентичен в обоих режимах; `worker_inbox->scope` и reactor-owned листенер/CID-steering не трогаются. Непрерывность листенера автоматическая. Fan-out — тот же filesystem-broadcast. **Ноль изменений.**

- **Tier C (hard) — квиесценция идёт через ПРОДЮСЕРА, не листенер.** `pause_listeners` — no-op (листенера нет). Нужно: (1) `worker_registry_disable_slot(id)` / `enable_slot(id)` — **их сегодня НЕТ** (в `worker_registry.h` только `create/publish/add/at/pick/least_busy/free`, unpublish отсутствует); (2) inbox «drain-mode», чтобы `worker_inbox_post` возвращал full/dead для перезагружаемого воркера → sticky-home+spill реактора перемаршрутизирует на пиров (как при «worker backs up or dies»). Затем `#74` drain по server_scope воркера, hook + invalidate, `enable_slot`. Пересоздание inbox обязано соблюдать контракт «producers quiesced, consumer-thread-only» (`worker_inbox.h:31-33/58`) — иначе реактор (другой поток) дереференсит освобождённый inbox → UAF. Reactor-owned листенер + CID-группы — parent/reactor-state, переживают респавн нетронутыми (непрерывность автоматическая).

Reactor-pool Tier C — **follow-on**, требует нового registry/inbox ABI поверх Tier C.

---

## 9. Что переживает reload

| Сущность | Tier A (soft, in-place) | Tier C (hard, respawn потока) |
|----------|-------------------------|-------------------------------|
| Сам поток воркера | Живёт | **Умирает** и пересоздаётся |
| Per-thread `CG/EG` (framework + named app классы) | Живут (объявлены раз) | Теряются → чистые, перекомпиляция |
| DB-пулы, prepared-stmt cache (`pool_stmt_cache`) | Живут (factory ОБЯЗАН переиспользовать, не пересоздавать) | **Теряются**, ребилдятся с нуля |
| Per-thread encoder pool (gzip LIFO), static/precomp кэши, interned strings | Живут | Теряются, прогреваются заново |
| DI-синглтоны bootloader'а | Живут | Теряются, ребилдятся |
| Listener fd | Не трогаются | Не трогаются (parent-owned / handoff, §4) |
| opcache SHM | Общий, per-file invalidate | Общий, перекомпиляция читает новый исходник |
| `protocol_handlers` | Живут (свопается только контент слота) | Перерегистрируются на свежем потоке через worker-init hook |
| server_scope | Не трогается | Минтится заново в `start()` |
| In-flight запросы | Не дренируются (каждый доигрывает свою версию) | Дренируются (`#74`), tail сверх grace — cancel |
| WS/SSE | Крутят старый код до close | Согнаны `1001`/финал SSE до cancel |

**Ключевой honest-tradeoff:** Tier A сохраняет ВСЁ thread-local → его потолок в том, что named-class-код не меняется. Tier C меняет named-class-код → его цена в том, что всё thread-local (пулы, кэши, синглтоны) отстраивается заново на каждом прокатанном воркере (N последовательных ребилдов → всплеск latency/числа коннектов). Дорогое состояние, которое должно переживать reload, надо выносить в process-shared (pemalloc/SHM) структуры — сегодняшний bootloader этого не разделяет.

---

## 10. Новый API

Соглашения проекта: **setter-not-INI**, enum-cases **UPPER_CASE**, get/set-симметрия как у существующего `getBootloader()/setBootloader()`.

### Tier A (userland-first, минимум C)

```php
// stubs/HttpServerConfig.php
enum ReloadMode {
    case SOFT;   // in-place slot swap (Tier A) — routes/wiring/config/closures
    case HARD;   // fresh-thread respawn (Tier C) — named-class code
}

final class HttpServerConfig {
    // Обёртка над Reloader-boilerplate: регистрирует стабильный делегат-хендлер,
    // спавнит watcher/SIGHUP-корутины в bootloader top-level scope.
    public function enableHotReload(
        array $watchPaths,
        ReloadMode $mode = ReloadMode::SOFT,
        ?\Closure $onReload = null,
        int $debounceMs = 300,
    ): static;

    // (для будущего Tier C) hook, вызываемый на КАЖДОМ свежем потоке против
    // свежескомпилированного кода — закрывает «bootloader без server ref».
    public function setWorkerInit(?\Closure $hook): static;   // fn(HttpServer $worker): void
    public function getWorkerInit(): ?\Closure;

    public function setReloadStaggerMs(int $ms): static;      // Tier C rolling offset
}
```

Userland-контракт SOFT-модуля (документируется, опционально — firewall): без top-level `class`/`function` в перезагружаемом поддереве; каждый модуль заканчивается `return <callable|object>`; дети через plain `require` (не `require_once`); можно `use` стабильные framework-классы.

### Tier C (server-C + php-async ABI)

```php
final class HttpServer {
    // admin/programmatic триггер, из top-level trigger-корутины (НЕ из request-хендлера
    // в epoch-scope). Возвращается после завершения reload'а ЭТОГО воркера.
    public function reload(array $changedFiles = [], bool $shedStreams = false): void;
}

final class Response {
    // кастом shed-поведение для SSE/долгих ответов при HARD-reload
    public function onGoingAway(\Closure $handler): void;
}
```

Сигналы/knob'ы: SIGHUP — чисто userland-паттерн (`Async\signal(Async\Signal::SIGHUP)`), **без C-обработчика**. Никаких INI — всё через сеттеры `HttpServerConfig` (прецедент `h2_static_budget_max`).

---

## 11. Блокеры и зависимости, фазировка

**Жёсткие блокеры Tier C (все сверены):**

1. **Нет ThreadPool respawn ABI.** Vtable = `{close, dispose, submit_internal}` (`Zend/zend_async_API.h:2007-2011`), fixed `worker_count`, one-time create-loop. `close()` рушит **общий** task_channel → выходят ВСЕ воркеры. Каждый server-воркер припаркован в `start()` на весь срок (`src/http_server_class.c:1819`) и никогда не возвращается в receive-цикл за второй задачей. → нужен **новый php-async ABI**: `respawn_worker(pool, idx)` + per-worker poison-pill (выход одного воркера без закрытия общего канала).

2. **Нет cross-thread parent→worker wake.** Это `#11`/`#72`: `channel.send/receive` суспендят, `poll_cb` — не корутина, неблокирующего cross-thread канала нет. Сегодня есть только worker→parent (`pool_worker_done_cb`, `src/http_server_class.c:1858-1871`). → нужна ABI-надстройка.

3. **transfer-while-running guard.** Доставка свежего clone в новый воркер (шаг 8) упирается в `Cannot transfer HttpServer while running` (`src/http_server_class.c:4246-4250`); `src->running==true` весь срок пула. → снять guard для reload-пути ИЛИ альт-доставка (re-freeze shared config + свежий transit, не триггерящий guard).

4. **Одноразовость завершения пула.** `pool_worker_done_cb` декрементит `st->pending`; при 0 будит parent, который **сносит весь пул** (`running=false`, `reactor_pool_down`, `efree(st)`, `DELREF pool`, `src/http_server_class.c:2639-2656`). → нужен per-worker re-arm `pending`/`all_done` на каждый новый future.

5. **Bootloader-failure рушит весь пул.** Любая ошибка boot'а свежего воркера (parse error от half-saved файла, `Cannot redeclare`, повторная миграция) зовёт `thread_pool_close` → закрывает общий channel → **весь процесс падает** (`php-src/ext/async/thread_pool.c:186-256`). → один плохой save превращает per-slot reload в full-outage. Нужна изоляция boot-ошибок от общего канала.

6. **Нет per-worker реестра соединений** для WS/SSE-shed и keep-alive-fixup (`src/http_server_class.c:967-972`). → нужна отдельная C-структура.

7. **Нет registry unpublish** для reactor-pool (`worker_registry.h`). → follow-on ABI.

**Фазировка:**

- **Phase 0 (MVP, отгружается сейчас):** Tier A userland `Reloader` + `HttpServerConfig::enableHotReload(SOFT)`. Ноль C для ядра; опционально — маленький C-сахар (встроенный стабильный диспетчер + `setReloadTarget`). Покрывает dev hot-loop + config/routing/closure-reload.
- **Phase 1 (server-only C, small):** непрерывность листенера через parent-owned fd; per-worker реестр соединений; WS/SSE shed-hook `1001`/финал + `Response::onGoingAway`; `setWorkerInit(HttpServer)`. Плюс: **process-level Tier B** blue-green как рекомендуемый способ named-class-reload (ноль нового server-ABI, уже работает через REUSEPORT).
- **Phase 2 (php-async ABI, XL):** `respawn_worker` + per-worker channel/poison-pill + cross-thread wake (`#11`/`#72`) + снятие transfer-guard + per-worker completion re-arm → полноценный in-process Tier C rolling respawn. Разблокирует и `#11`.
- **Phase 3 (reactor-pool ABI):** `worker_registry_disable_slot/enable_slot` + inbox drain-mode → Tier C для `#80`.

---

## 12. Открытые вопросы и выжившие риски

1. **Named-class потолок Tier A (verified).** Изменение тела уже объявленного `class/function` в живом потоке = ноль эффекта; re-`require` = `Cannot redeclare` (bailout, **не** `Throwable` — не ловится `try/catch`). Реальные PSR-4 приложения не вписываются без реструктуризации перезагружаемой поверхности в `return`-модули. Для named-class-изменений — Tier B/C.

2. **Unbounded per-thread рост (Tier A, verified).** Каждый reload минтит новое анонимное имя через `CG(rtd_key_counter)++` (монотонный per-thread), интернирует его (interned strings **никогда** не освобождаются за жизнь потока), добавляет перманентную запись в `EG(class_table)` + wasted opcache SHM. «Bounded leak» — мисноминация: рост монотонен по числу reload'ов. Ставит потолок на число reload'ов до неизбежного рестарта процесса.

3. **opcache wasted-SHM → недетерминированный рестарт (survives для всех tier'ов).** Частые invalidate растят wasted до `max_wasted_percentage` → opcache сам планирует рестарт-wipe (запрещённый нами primitive), под ZTS потенциально UAF через слепой к siblings fcntl-гейт. Смягчение — invalidate только реально изменённых файлов + периодический process-restart.

4. **Debounce-грабли (verified критиком).** `Async\delay()` в теле `foreach` watcher'а **суспендит** foreach → это per-event throttle, НЕ debounce: N файлов = N reload'ов по ~300мс, каждый ребилдит граф из half-written набора. Debounce обязан быть на таймере ВНЕ приёма событий (отдельная корутина-таймер сбрасывается на каждом событии, reload только после тишины).

5. **`recursive:true` не работает на Linux (verified критиком).** `UV_FS_EVENT_RECURSIVE` реализован только на macOS/Windows; на Linux/inotify флаг молча игнорируется — видны только прямые дети каталога. Dev-триггер по вложенному дереву **молча мёртв**. Watch'им плоский каталог/sentinel, а не всё дерево; или обходим subdir'ы вручную несколькими watcher'ами.

6. **N воркеров × FileSystemWatcher = inotify-thrash / гонки invalidate.** N потоков одного процесса watch'ат те же пути; при sentinel-broadcast каждый независимо `invalidate` + перекомпилирует одни файлы — до N избыточных discard/recompile и N× wasted-рост на deploy, плюс контеншн `zend_shared_alloc_lock` (head-of-line-блокировка любого запроса, которому нужен compile). Watch'ить sentinel в одном воркере/parent'е и веерить — предпочтительно.

7. **SIGHUP → смерть процесса (verified критиком).** Окно между резолвом one-shot `Async\signal` и re-arm без хендла → дефолт SIGHUP = Terminate → второй HUP в это окно убивает весь процесс. Prod fan-out — filesystem-broadcast, не SIGHUP-как-единственный-триггер.

8. **Staggered rolling без interlock'а (Tier C).** Cross-thread барьера нет (`#72`); если `stagger < drain_duration` (grace — секунды), несколько воркеров паузятся разом → просадка ёмкости. Одиночный process-SIGHUP может разбудить лишь один поток. Нужен либо централизованный parent-координатор (упирается в cross-thread wake, блокер §11.2), либо cons' с приблизительностью ролла.

9. **WS/SSE coroutine-exit (Tier C).** Enqueue `1001`/SSE-финала **не разматывает** корутину в `foreach recv()`; нужно ещё закрыть/отменить read-сторону, иначе Phase2 hard-cancel всё равно. И для перечисления нужен per-worker реестр соединений (нет сегодня, §11.6). SSE нельзя pre-parent в durable_scope (SSE-ность известна только в рантайме).

10. **Config/pool-reload частично недостижим в Tier A.** Перезагружаемый контракт запрещает named-объявления, а config-driven pool sizing/DSN живёт именно в non-reloadable named-слое. Наивный stateful-код в reloadable-слое double-init'ит пулы/течёт коннектами на каждом reload'е (старый пул ещё держат in-flight запросы, никогда не закрывается). Реально перезагружаемая поверхность Tier A сужается до route→closure карт и инертных config-массивов.

11. **Cross-version корректность.** Старый in-flight код и новый код одновременно трогают общее mutable-состояние (синглтоны, пулы, session store). Tier A: factory ОБЯЗАН переиспользовать выжившие синглтоны. Tier C: во время ролла старо- и ново-кодовые воркеры обслуживают конкурентно — breaking schema/protocol change посреди ролла может побить клиента, чьи запросы попали и на старый, и на новый воркер. Держать reload'ы backward-compatible по форме данных.

---

## Итог одной строкой

Отгружаем **Tier A (userland SOFT-reload)** сейчас как MVP `#93` для dev-hot-loop и config/routing/closure-reload (ноль C, работает в живом потоке, честный потолок — named-классы); named-class-изменения в prod закрываем **Tier B (process-level blue-green через уже-существующий SO_REUSEPORT)**; полноценный **in-process per-worker rolling respawn (Tier C)** проектируем, но строим только после нового php-async ThreadPool ABI (`respawn_worker` + cross-thread wake + снятие transfer-guard) — это же разблокирует `#11`. Design 2 (in-place epoch-swap) отклоняем: `#74 cancel()` терминален, keep-alive гоняется в отменённый scope, нет списка соединений, SSE не pre-parent'ится. opcache — только `opcache_invalidate($f,true)`, никогда `opcache_reset()`.

---

## Приложение A. Референс: как это делает Swoole/OpenSwoole

Swoole решает ровно `#93` и служит хорошей калибровкой — но работает гладко **именно потому, что его воркеры — процессы, а не потоки**. Это подсвечивает нашу главную асимметрию.

**Модель процессов:** `Master` (владеет listen-сокетами + reactor-треды, `accept`) → `Manager` (форкает и пасёт воркеров) → `Worker`/`Task worker` (PHP). Слушающий fd живёт в Master, воркеры — форкнутые дети, `accept`'ящие из общего сокета.

**`$server->reload()` / `kill -USR1 <master_pid>`** (USR2 — только task):
- Перезапускает **только Worker/Task процессы**; Master и Manager живут → **listen-сокет не пересоздаётся, соединения не рвутся**.
- **Graceful + rolling:** Manager убивает воркера только когда тот дообработал текущие запросы, по одному, затем форкает свежего. `max_wait_time` (дефолт 3с) — потолок, дальше форс-килл + `onWorkerStop`.
- **`reload_async`** (вкл. по умолчанию, всегда при корутинах): воркер до-обрабатывает все pending события/корутины, потом выходит и переформивается — корутино-safe drain.
- Reload **дебаунсится**: второй во время активного отбрасывается. Хуки `BeforeReload`/`AfterReload`. В `Base mode` (без Manager) reload рвёт соединения.

**Знаменитое ограничение:** перезагружается только код, `require`'нутый **после** `onWorkerStart`. Всё, загруженное в Master до `$server->start()`, впечатано в форкнутый образ и **не перезагружается** без полного рестарта. Свежий форк = чистая память процесса → воркер заново `require`'ит app-код, opcache с `validate_timestamps` подхватывает изменённые файлы.

**Вотчер — userland:** `ext-inotify` следит за included-файлами, на изменение зовёт `$server->reload()`. Дев-онли.

### Как это ложится на нас

| | Swoole | TrueAsync-server |
|---|---|---|
| Изоляция воркера | **процесс** (fork) | **поток** (ThreadPool) |
| Владелец listener | Master (отд. процесс) | pool-parent + shared-fd / REUSEPORT |
| Кто катит reload | **Manager** + signals к процессам | **отсутствует → блокер #11 / §11.2** |
| Свежие таблицы символов | новая память процесса | новые per-thread TSRM `CG/EG` (Tier C) |
| opcache SHM | общий на форки | общий на потоки (то же, безопасно при invalidate) |
| «код до `start()` не релоадится» | код в Master | transferred-once snapshot + handler-замыкания (§1.3) |
| Graceful drain | `reload_async` + `max_wait_time` | наш `#74` drain + `getShutdownTimeout()` |

**Выводы, которые Swoole подтверждает:**
1. Корректный ответ на HARD-reload в persistent-рантайме — **свежий воркер + сохранение listener-fd + rolling + graceful drain с потолком по времени**. У нас это Tier C (in-process respawn потока) и Tier B (process-level blue-green).
2. То, что у Swoole бесплатно даёт `Manager`-процесс + Unix-сигналы к процессам (cross-process контроль жизненного цикла воркера), у нас **отсутствует** — это блокер `#11`/`#72` (§11.1–11.2). Swoole не платит за него, потому что процессы.
3. Их «код до `start()` не перезагружаем» = наш «handler-замыкания и config-snapshot трансферятся один раз» (§1.3). Одна и та же природа: то, что впечатано в образ воркера до его цикла, не релоадится без пере-создания воркера.
4. **`max_wait_time`, дебаунс reload'ов и хуки `BeforeReload/AfterReload`** — забираем в наш API: потолок grace с форс-cancel (у нас уже есть `getShutdownTimeout()` как grace, §3B шаг 5), single-flight reload guard, `onReload`-колбэк.

**Ключевая асимметрия одной строкой:** Swoole `reload()` = «kill+refork процесс за стабильным Master-листенером». Наш эквивалент = «teardown+respawn поток за parent-owned fd» — семантически то же, но требует ABI (`respawn_worker` + cross-thread wake), которого у ThreadPool сегодня нет; до него named-class-reload в prod закрывается процессным blue-green (Tier B) поверх уже существующего `SO_REUSEPORT`.

**Sources:** [OpenSwoole reload()](https://openswoole.com/docs/modules/swoole-server-reload) · [OpenSwoole config: reload_async / max_wait_time](https://openswoole.com/docs/modules/swoole-server/configuration) · [mezzio-swoole hot code reload](https://docs.mezzio.dev/mezzio-swoole/v2/hot-code-reload/)
