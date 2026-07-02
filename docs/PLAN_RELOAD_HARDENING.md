# PLAN: закрытие дыр в `ThreadPool::reload()` (channel-swap) — #93

> Входные данные: ревью запушенной ветки `fs-watcher-recursive-linux` (php-async `df5dbfa`,
> php-src `555ea923ff`). Ядро дизайна (channel-swap, когорты, синхронный захват канала)
> подтверждено чистым; дыры — на failure-путях. Этот план закрывает их ВСЕ и прогоняет
> алгоритм по 13 сценариям.

---

## 1. Находки, которые закрываем

| # | Дыра | Где | Тяжесть |
|---|------|-----|---------|
| H1 | Bailout-путь воркера не шлёт reload-токен: `done:` внутри `zend_try` (thread_pool.c:601), bailout прыгает в `zend_catch` (:640) мимо него → `reload()` может зависнуть | thread_pool.c | major |
| H2 | Нет защиты от конкурентного/повторного `reload()`: второй вызов перезаписывает `pool->reload_notify` → токены первого уходят во второй, первый виснет | thread_pool.c:884 | major |
| H3 | Токен шлёт ЛЮБОЙ выходящий воркер, не только старая когорта: свежая замена, умершая в bootloader'е (goto done на :221/:255/:268), постит токен → досрочный счёт; последний старый шлёт в закрытый notify → `ThreadChannelException` в выходящем воркере поверх scope-teardown | thread_pool.c:607-614 | major |
| H4 | `disposing` — мёртвое поле (пишется libuv_reactor.c:3066, не читается нигде) | libuv_reactor.c/.h | minor |
| H5 | Эмулированный recursive-вотчер не переживает stop()→start(): узлы остаются в `watch_dirs`, dedup в `add_dir` возвращает true без повторного `uv_fs_event_start` | libuv_reactor.c | minor/латентно |
| H6 | PLAN_HOTRELOAD.md противоречив: A.3 переписан на channel-swap, но шапка (строка 5) и фазовый план (строка 105) всё ещё про отвергнутый `respawn_worker` | docs (server) | minor |
| H7 | Тестовое покрытие: есть только 070 (single reload). Нет double/stress/in-flight/конкурентного | tests | minor |

Дополнительно (мелочь, по пути): `reload()` вне корутины деградирует в 2N-спайк без
ожидания; PHP-метод молча no-op при `reload == NULL` (старый ABI).

## 2. Целевые инварианты

- **I1 (liveness):** `reload()` ВСЕГДА завершается — при bailout'ах воркеров, падениях
  bootloader'а замен, `close()` пула посреди reload, отмене reload-корутины.
- **I2 (точный счёт):** токены шлёт РОВНО старая когорта этого reload — никто другой.
  Ни переполнения notify, ни поздних send'ов в закрытый канал (в штатных режимах).
- **I3 (чистый выход воркера):** exit-путь воркера никогда не блокируется на token-send,
  не оставляет `EG(exception)`, не трогает освобождённую память.
- **I4 (сериализация):** один reload на пул в моменте; второй вызов — явная ошибка.
- **I5 (задачи, уже держится):** буфер старого канала дренируется старой когортой до
  выхода (receive отдаёт буфер ДО проверки closed — thread_channel.c:188); новые submit'ы
  идут в новый канал.
- **I6 (close-interplay):** `close()`/dispose пула посреди reload → закрытый пул, без
  зависаний и утечек.

## 3. Итоговый алгоритм

### 3.1. Состояние на пуле (thread_pool.h)

```c
/* Кросс-поточные (читают выходящие воркеры): */
zend_atomic_ptr reload_notify;   /* есть (:70). Канал токенов активной ротации, NULL вне её */
zend_atomic_ptr reload_old;      /* НОВОЕ. Канал, который ЭТА ротация закрыла (identity-gate) */
zend_atomic_ptr task_channel;    /* БЫЛО plain — теперь atomic (вердикт Q2): bailout-воркер
                                    читает его из чужого потока в thread_pool_close/drain */

/* Owner-thread-only (все вызовы reload() идут с потока-владельца пула,
   корутины не вытесняются вне suspension points — атомики не нужны): */
bool     reload_in_progress;     /* НОВОЕ. Активная ротация */
uint64_t rotations_started;      /* НОВОЕ. Счётчики для схлопывания очереди */
uint64_t rotations_completed;
HashTable reload_waiters;        /* НОВОЕ. per-waiter триггеры очереди (паттерн
                                    receiver_triggers из thread_channel.c) */
async_thread_channel_t *orphan_notify; /* НОВОЕ. Хвосты ПРЕРВАННОЙ ротации; disposed */
async_thread_channel_t *orphan_old;    /* на старте следующей ротации / в dispose пула */
```

### 3.2. Сторона воркера — ОДНА точка отправки токена

Убрать блок из `done:` (:601-616). Вставить ПОСЛЕ `zend_end_try` + bailout-блока,
между dispose slot_event (:663) и `ZEND_THREAD_POOL_DELREF` (:666) — эта точка
достигается **всеми** путями выхода: нормальным И bailout (закрывает H1).

```c
/* Identity-gate: токен шлёт только воркер, чей канал закрыл ИМЕННО этот reload.
 * Свежая замена (channel == новый) и стрэгглер прошлого reload не проходят. */
async_thread_channel_t *notify =
        (async_thread_channel_t *) zend_atomic_ptr_load_ex(&pool->reload_notify);

if (UNEXPECTED(notify != NULL) &&
        zend_atomic_ptr_load_ex(&pool->reload_old) == (void *) channel) {
    zval token;
    ZVAL_TRUE(&token);

    if (UNEXPECTED(false == notify->channel.send(&notify->channel, &token))) {
        zend_clear_exception(); /* оборона: по точному счёту недостижимо */
    }
}
```

Свойства:
- **Сравнение указателей ДО разыменования notify** — стейл-указатель никогда не трогается.
- send не может заблокироваться: capacity == n, отправителей ровно n (I2 ⇒ I3).
- send не может throw'нуть в штатном режиме; если всё же (оборона) — clear, воркер
  выходит чистым (I3).
- Точка после AFTER_MAIN ⇒ токен означает «воркер ПОЛНОСТЬЮ завершён» — более сильная
  гарантия для будущего `HttpServer::reload()` (M3).

### 3.3. Сторона reload() (заменяет thread_pool.c:852-910)

Семантика (вердикт обсуждения): **сериализация + схлопывание**. Перекрывающиеся
вызовы НЕ бросают и НЕ игнорируются: они встают в очередь; после завершения активной
ротации ОДИН из ожидавших выполняет одну полную follow-up-ротацию, которая закрывает
гарантию всем ожидавшим сразу («когда мой reload() вернулся — каждый живой воркер
загружен не раньше момента моего вызова»). Поштучных/частичных reload'ов нет —
единица работы всегда полная ротация N воркеров.

```
reload(pool):
 1. if closed(pool)            -> return
 2. if нет текущей корутины    -> throw Error("reload() must be called from a coroutine")
 3. target = rotations_started + 1
    /* Ротация, стартовавшая ПОСЛЕ моего входа, выполняет мою гарантию. */
 4. while (reload_in_progress):
      ждать на per-waiter триггере (паттерн thread_channel: триггер в
      reload_waiters, resume_when + SUSPEND в zend_try/catch, удаление из
      таблицы + dispose на всех путях, включая bailout)
      if исключение (отмена в очереди) -> return с исключением (состояние не трогали)
      if rotations_completed >= target -> return        /* СХЛОПНУЛИСЬ ✓ */
      if closed(pool) -> return
 5. n = worker_count; if n <= 0 -> return
    /* Становимся ротатором */
    reload_in_progress = true; rotations_started++
 6. if orphan_notify/orphan_old -> close+dispose, NULL
    /* хвосты прерванной ротации; стрэгглеры в них уже не пишут (identity-gate
       мимо, поля NULL'ированы на аборте), память страхует thread-registry */
 7. old = task_channel
    new = channel_create(old->capacity); notify = channel_create(n)
    on-fail: dispose частичного, rotations_started--, in_progress=false,
             fire(waiters), throw
 8. atomic: reload_old = old; ЗАТЕМ reload_notify = notify   (seq_cst, C11)
 9. atomic: task_channel = new; close(old)
    /* старая когорта: дожёвывает буфер old (I5), потом receive==false -> выход */
10. for i in 0..n-1:
      a. if !closed(pool): start_worker(pool, i)      /* fail -> продолжаем, см. S5 */
      b. ok = notify.receive(&token)                  /* суспенд до токена */
         if ok: dtor(token); continue
         else (исключение: отмена ротатора) -> ABORT:
             atomic: reload_notify = NULL; ЗАТЕМ reload_old = NULL
             orphan_notify = notify; orphan_old = old  /* НЕ dispose: стрэгглеры,
                успевшие загрузить пару до NULL, дошлют в буфер (cap=n) */
             rotations_completed НЕ инкрементим (гарантия не доставлена)
             in_progress=false; fire(waiters)          /* один из них доротирует
                и заодно вылечит частичную когорту */
             return с исключением (пропагируем, НЕ глотаем)
11. /* точный счёт: все n старых отправили и завершили send (mutex-ordering) */
    atomic: reload_notify = NULL; ЗАТЕМ reload_old = NULL
    close+dispose(notify); dispose(old)
    rotations_completed++; in_progress=false; fire(waiters)
```

Dispose пула: добавить `if (orphan_notify) close+dispose`. Плюс если на момент dispose
`reload_notify != NULL` (abort был последним действием перед dispose) — dispose и его.
Порядок безопасен: dispose пула случается при refcount==0, а КАЖДЫЙ воркер держит ref
до конца (DELREF :666 — ПОСЛЕ token-send) ⇒ пока есть потенциальный отправитель, пул
и его поля живы.

### 3.4. Почему identity-gate, а не проверка `IS_CLOSED(channel)`

Гейт «мой канал закрыт» пропускает лишних отправителей: при `close()` пула посреди
reload закрыт и НОВЫЙ канал → свежие воркеры тоже шлют → переполнение notify (cap=n),
блокировка выходящего воркера. Гейт «мой канал == reload_old» опирается на *identity*,
а не на состояние: отправители ≡ старая когорта ≡ ровно n, по построению. Все
переполнения/поздние-throw/ложные-счёты отпадают классом, а не заплатками.

Согласованность пары (notify, old) при чтении из воркера: пишутся old→notify,
чистятся notify→old (обратный порядок), оба seq_cst; одновременно активен максимум
один reload (I4) ⇒ если воркер увидел notify != NULL, то old валиден для этого reload.

### 3.5. PHP-метод

`reload == NULL` (чужой пул на ABI < 0.22): вместо молчаливого no-op —
`throw AsyncException("this thread pool does not support reload()")` (честно; наш пул
всегда имеет reload). Стуб: задокументировать исключения (повторный вызов, вне корутины).

## 4. Ментальный прогон (13 сценариев)

| # | Сценарий | Трасса | Исход |
|---|----------|--------|-------|
| S1 | Happy path: n=3, reload | swap→close(old)→3×(spawn+token)→cleanup | 3 свежих, старые вышли, каналы освобождены ✓ |
| S2 | Два reload'а подряд | K завершился полностью (точный счёт) → поля NULL → K+1 с чистого листа | ✓; тест 071 |
| S3 | In-flight длинная задача | старый воркер дожёвывает задачу → receive==false → выход → токен; reload ждёт ровно до этого | reload = max(остаток текущих задач); задача НЕ теряется ✓; тест 072 |
| S4 | submit ВО ВРЕМЯ reload (reload-корутина в суспенде) | submit с owner-потока видит `task_channel == new` → буфер нового → исполнит свежий воркер | ✓; тест 073 |
| S5 | Bootloader замены падает/exit() | замена проходит done:/выход, но её channel == new ≠ reload_old → токена НЕТ; счёт честный; пул деградирует до n−1 (та же семантика, что при первичном спавне) | нет досрочного счёта, нет позднего throw ✓; тест 075 |
| S6 | Старый воркер уходит через bailout посреди reload | token-send теперь ПОСЛЕ zend_end_try → токен уходит и с bailout-пути; bailout-блок дополнительно закрывает пул → см. S7 | reload завершается ✓ (H1 закрыт) |
| S7 | `close()` пула посреди reload | close закрывает task_channel (=new; thread_pool_close — подтверждено кодом) → свежие выходят БЕЗ токенов (gate); старые выходят по своему (уже закрытому) old → все n токенов приходят; шаг 9a перестаёт спавнить | reload возвращается, пул закрыт, без зависаний ✓ |
| S8 | Отмена reload-корутины в receive | ABORT: поля NULL → orphan-слоты, `completed` НЕ растёт, waiters разбужены → один доротирует (и лечит частичную когорту), исключение пропагируется | отмена не глотается; гарантия ожидавших доставляется follow-up'ом ✓ |
| S9 | 2-3 reload() во время активной ротации | все встают в очередь (target = started+1); после ротации №1 один waiter выполняет ОДНУ полную ротацию №2; остальные видят completed ≥ target → return | «reload ×3» = 2 полные ротации, не 3 и не поштучно ✓ (H2 закрыт); тест 074 |
| S10 | reload() вне корутины | guard шага 2 → Error сразу, до каких-либо свапов | защита C-вызывателей (из userland main — уже корутина, недостижимо) ✓ |
| S11 | Стрэгглер поколения K при активном K+1 | загрузил notify(K+1) — non-NULL; gate: его канал == K-old ≠ (K+1)-old → skip, notify не разыменован дальше gate'а | нулевой cross-talk между поколениями ✓ |
| S12 | reload на закрытом пуле | шаг 1 → return | как сейчас ✓ |
| S13 | dispose пула после ABORT'нутого reload | воркеры держат pool-ref до последней строки → dispose позже любого send; dispose чистит orphan_notify (+reload_notify, если остался) | нет утечки, нет UAF ✓ |

Подстраховки вне точного счёта (оборона в глубину): thread-registry держит память
каждого канала до shutdown создавшего потока (thread_channel.c:73-89) — даже
теоретический стейл-send упирается в closed-check под мьютексом, а не в freed memory.

## 5. Изменения по файлам

### php-async (ветка `fs-watcher-recursive-linux`)

1. **thread_pool.h** — `reload_old`, `reload_active`, `orphan_notify` (§3.1).
2. **thread_pool.c**
   - убрать token-send из `done:` (:601-616); вставить единую точку после bailout-блока (§3.2);
   - переписать `thread_pool_reload` (:852-910) по §3.3;
   - init новых полей в `async_thread_pool_create` (:976 рядом);
   - dispose пула: чистка orphan_notify/reload_notify;
   - `METHOD(reload)` (:1566): throw вместо тихого no-op; guard "вне корутины".
3. **thread_pool.stub.php** — документировать исключения reload(). Перегенерить arginfo.
4. **libuv_reactor.h/.c** — H4: удалить `disposing`; H5: в `libuv_filesystem_start`
   (recursive-ветка) — если `watch_dir_count > 0` (рестарт после stop), перевзвести
   `uv_fs_event_start` на каждом существующем узле вместо полного add_dir-пути.
5. **Тесты** (H7):
   - 071-reload-double: два reload подряд → boots == 3n, пул служит.
   - 072-reload-inflight: длинная задача переживает reload, результат корректен.
   - 073-reload-submit-during: submit из другой корутины, пока reload в суспенде.
   - 074-reload-coalesce: 3 перекрывающихся reload (n=1, первый затянут in-flight
     задачей) → ровно 2 ротации (boots == 3), все три вызова вернулись без исключений.
   - 075-reload-bootloader-fail: bootloader замен падает → reload возвращается,
     пул деградирован, без крэша/зависания (гоняет S5-gate).
   - 076-reload-stress: 10× reload под потоком submit'ов (в CI всегда — вердикт Q4);
     прогнать локально под valgrind.
   - (077 отменён: из userland main — уже корутина, «вне корутины» недостижимо;
     guard остаётся как защита C-вызывателей.)
   - bailout-mid-reload (S6) детерминированно из phpt не взводится — покрывается
     code-path-ревью + 076; отметить в тесте комментарием.

### php-src (ветка `fs-watcher-recursive-linux`)

6. **Zend/zend_async_API.h** — комментарий к `reload`: дополнить семантикой ошибок
   (throw при повторном вызове/вне корутины). Сигнатура НЕ меняется, ABI остаётся 0.22
   (в релиз ещё не уходил).

### true-async/server (ветка `93-hotreload-design`)

7. **docs/PLAN_HOTRELOAD.md** (H6): шапку (строка 5) и «Фаза 1» (строка 105) привести
   к channel-swap; упоминания `respawn_worker` в исторических разделах пометить
   «(историческое, отвергнуто)» — не переписывать анализ.

### Память/чекпоинт

8. Обновить `project_93_hotreload_design.md`: убрать устаревшее «нужен новый коммит
   revert+reload», честные test-claims, состояние после этого плана.

## 5.1. ~~ВОССТАНОВЛЕНИЕ~~ — ✅ ВОССТАНОВЛЕНО 2026-07-02

Ветки возвращены (php-async `fs-watcher-recursive-linux`, server `93-hotreload-design`),
стэши подняты, правки libuv_reactor.c на месте (потери не было — параллельный агент
вернул всё сам), H6 в PLAN_HOTRELOAD.md сделан. Раздел ниже — история.

<details><summary>исходная процедура (неактуальна)</summary>

## (история) ВОССТАНОВЛЕНИЕ (2026-07-02): работа реализована, но сташнута параллельной сессией

Всё из §3/§5 БЫЛО реализовано, собрано и проверено (suites 91/91; valgrind на
070-076: 7/7, 0 leaked). Затем параллельная сессия (Ctrl+C/pcntl, PR server#94)
переключила `~/spc-work/php-async` на `fix-signal-pcntl-clobber`, сташнув правки.

**Где что лежит:**
- `stash@{0}` «wip hot-reload (fs-watcher): reactor.h + thread_pool» —
  thread_pool.c (+211/-46), thread_pool.stub.php, thread_pool_arginfo.h,
  libuv_reactor.h (снятие поля `disposing`).
- `stash@{1}` «wip thread_pool.h (fs-watcher)» — thread_pool.h (новые поля).
- Тесты 071-076 — untracked, лежат в tests/thread_pool/ ✓.
- php-src: НЕ тронут, ветка fs-watcher-recursive-linux, правка комментария
  в Zend/zend_async_API.h в рабочем дереве ✓.
- **ПОТЕРЯНО (не в стэшах): 2 правки libuv_reactor.c** — восстановить по диффу ниже.

**Процедура:** дождаться конца параллельной сессии → `git checkout
fs-watcher-recursive-linux` → `git stash pop` ×2 → восстановить libuv_reactor.c
(ниже; без этого сборка сломана — .h из стэша уже без `disposing`) → make →
suites + valgrind.

**Правка 1 (H5, libuv_filesystem_start, recursive-ветка):** заменить
`if (fs_event->recursive_emulated) { if (UNEXPECTED(false == async_fs_watch_add_dir(...)))`
на:

```c
	if (fs_event->recursive_emulated) {
		if (fs_event->watch_dir_count > 0) {
			/* Restart after stop(): re-arm every surviving node; a directory
			 * gone meanwhile is dropped the same way remove_subtree drops it. */
			uint32_t i = 0;

			while (i < fs_event->watch_dir_count) {
				async_fs_watch_dir_t *node = fs_event->watch_dirs[i];

				if (UNEXPECTED(uv_fs_event_start(&node->handle, async_fs_watch_dir_cb,
						ZSTR_VAL(node->abs_path), 0) < 0)) {
					fs_event->watch_dirs[i] = fs_event->watch_dirs[--fs_event->watch_dir_count];
					uv_close((uv_handle_t *) &node->handle, async_fs_watch_dir_orphan_close_cb);
					continue;
				}

				i++;
			}
		} else if (UNEXPECTED(false ==
				async_fs_watch_add_dir(fs_event, fs_event->event.path, ZSTR_EMPTY_ALLOC()))) {
			async_throw_error("Failed to start recursive filesystem watch on %s", ZSTR_VAL(fs_event->event.path));
			return false;
		}

		event->loop_ref_count++;
		ZEND_ASYNC_INCREASE_EVENT_COUNT(event);
		return true;
	}
```

**Правка 2 (H4, libuv_filesystem_dispose):** удалить строку
`fs_event->disposing = true;` (поле уже снято из libuv_reactor.h стэшем).

**Не сделано (заблокировано ветками):** H6 — правки docs/PLAN_HOTRELOAD.md
(шапка стр.5 + «Фаза 1» стр.105 всё ещё про respawn_worker) — файл на ветке
`93-hotreload-design` server-репы, дерево занято ctrlc-сессией.

</details>

## 6. Вопросы — РЕШЕНЫ (вердикты 2026-07-02)

1. **Перекрывающийся reload** → ни throw, ни игнор: **сериализация + схлопывание**
   (§3.3). Игнор терял бы новейший код; параллельно — бессмысленно; поштучных
   reload'ов нет — всегда полная ротация N.
2. **`task_channel` → atomic ptr** → ДА. Конкретный сценарий: bailout-воркер из
   своего потока читает `task_channel` в `thread_pool_close` во время свапа →
   может закрыть старый канал вместо нового → свежие воркеры навсегда запаркованы
   в receive на незакрытом канале.
3. **H5** → фиксить кодом: при рестарте перевзводить `uv_fs_event_start` на
   существующих узлах (упавшие узлы — unlink + orphan-close).
4. **076 stress** → гонять в CI всегда.
