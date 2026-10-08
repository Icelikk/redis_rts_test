# Redis RTS Test 

## Вход

`loader` генерит события и пишет их в **Redis Stream**:

- **Stream:** `sensor_data`
- **Поля каждого сообщения:**
  - `tag_id` — идентификатор тега/датчика
  - `value` — значение измерения
  - `event_time` — "время датчика" (для проверки рассинхрона)
  - `quality` — качество (пока просто `"OK"`)
- **Stream ID** задаёт Redis (через `XADD ... "*"`) и выглядит как `<ms>-<seq>`, например: `1790327444006-0`

## Обработка

`worker` читает Stream через **consumer group** (режим *at-least-once*) и пишет результаты обратно в Redis:

- **Raw ряд** (по каждому тегу):
  - `ts:raw:<tag>`
- **Downsampling (агрегаты)** по правилам RTS, автоматически:
  - 20 секунд: `ts:avg/min/max/cnt:20s:<tag>`
  - 1 минута: `ts:avg/min/max/cnt:1m:<tag>`
- **Последнее значение** по тегу (быстрый "текущий статус"):
  - Hash: `light:current_values`
- **Курсор времени** (до какого момента данные обработаны):
  - String: `rts:last_ms`

## Визуализация

- **RedisInsight** (UI) — браузерный интерфейс к Redis
- `redis-cli` — быстрые проверки

---

## Время

У нас есть два времени:

1. **arrival time** — время прихода события в Redis
   Мы берём его из **Stream ID** (`<ms>-<seq>`) и используем как timestamp в RTS:
   - `TS.ADD ts:raw:<tag> <arrival_ms> <value> ...`

2. **event_time** — время "датчика", которое лежит в поле сообщения
   Мы используем его только для проверки рассинхронизации:
   - `delta_ms = arrival_ms - event_time_ms`
   - если `abs(delta_ms)` больше порога → помечаем `BAD_TIME` (опционально)

---

## Очистка

- **Stream** (`sensor_data`) режется через **XTRIM** по времени:
  - `XTRIM sensor_data MINID ~ <cut_ms>-0`
  - обычно держим последние ~20 минут
- **TimeSeries** режутся встроенным **RETENTION** (задаётся в `TS.CREATE`):
  - raw по умолчанию ~10 минут
  - агрегаты 20s ~1 час
  - агрегаты 1m ~1 день

---

## Требования

- Docker Engine
- Docker Compose **v2** (команда `docker compose ...`)

Проверка:

```bash
docker compose version
```
# Быстрый старт

### 1) Клонировать проект

```bash
git clone https://github.com/Icelikk/redis_rts_test
cd redis_rts_test
```

### 2) Поднять контейнеры

```bash
docker compose up -d --build
docker compose ps 
```
## Если ошибка: `failed to bind host port 0.0.0.0:6379 ... address already in use`

**Что это значит:**

На хосте уже работает Redis (или другой сервис) и занял порт `6379`, поэтому Docker не может пробросить `6379:6379`.

**Решение 1 — поменять проброс порта Redis в `docker-compose.yaml`:**

```yaml
services:
  redis:
    ports:
      - "6380:6379"
```

**Решение 2 — вообще убрать проброс `6379` наружу**, если Redis с хоста не нужен: просто удалить из `docker-compose.yaml` строку в секции `ports`.
### 3) Зайти в dev-контейнер

Все сборки и скрипты запускаются внутри dev-контейнера :

```bash
docker compose exec dev-window bash
```

---

## Сборка

-  сборка делается вручную внутри контейнера в `/app/build`

**Сборка (внутри `dev-window`):**

```bash
mkdir build
cd /app/build
cmake ..
make -j$(nproc)
```

**Проверка, что бинарники есть:**

```bash
ls -la /app/build/loader /app/build/worker
```

---

## Запуск loader + worker

Внутри `dev-window`:

```bash
/app/script/run.sh
```


## Быстрые команды проверки (внутри `dev-window`)

### Stream живой

```bash
redis-cli -h redis XLEN sensor_data
redis-cli -h redis XINFO STREAM sensor_data
```

### Consumer group (lag/pending/последний прочитанный id)

```bash
redis-cli -h redis XINFO GROUPS sensor_data
redis-cli -h redis XPENDING sensor_data rts_group
```

### Hash "последнее значение"

```bash
redis-cli -h redis HLEN light:current_values
redis-cli -h redis HGET light:current_values 0
```

### Курсор времени

```bash
redis-cli -h redis GET rts:last_ms
```

### RTS: последние точки (пример для tag=0)

```bash
redis-cli -h redis TS.INFO ts:raw:0 | head
redis-cli -h redis TS.REVRANGE ts:avg:20s:0 - + COUNT 10
redis-cli -h redis TS.REVRANGE ts:cnt:20s:0 - + COUNT 10
```

---

## RedisInsight (UI)

### Если Docker запущен локально

Открыть: <http://localhost:5540>

Подключение базы в UI:

- **Host:** `redis`
- **Port:** `6379`
- **Username/Password:** пусто (если не включали ACL)

### Если Docker на удалённой машине (через SSH)

Подними туннель с локального ПК.

**Linux/macOS:**

```bash
ssh -N -L 5550:127.0.0.1:5540 <user>@<server_ip>
```

**Windows PowerShell:**

```powershell
ssh -N -L 5550:127.0.0.1:5540 <user>@<server_ip>
```

Открыть в браузере на локальном ПК: <http://localhost:5550>

Дальше в RedisInsight добавляешь базу так же:
<img width="1005" height="770" alt="изображение" src="https://github.com/user-attachments/assets/4576f144-7acb-4fff-bf90-bd5acc17673b" />

- **Host:** `redis`
- **Port:** `6379`

> Если локальный порт `5540` занят — используй `5550` (или любой свободный).

## Как смотреть RTS (RedisTimeSeries) в RedisInsight

В некоторых версиях RedisInsight TimeSeries не всегда красиво рисуется “как график” прямо в Browser, поэтому самый надёжный способ — смотреть точки через **Workbench / CLI**.

### Открой Workbench
В RedisInsight:
- выбери подключённую базу Redis
- перейди во вкладку **Workbench** (или **CLI** — название зависит от версии)
---
### Модуль TimeSeries загружен

```redis
MODULE LIST
```

### Информация по raw-ряду (пример для `tag=0`)

```redis
TS.INFO ts:raw:0
```

### Последние 10 точек из raw

```redis
TS.REVRANGE ts:raw:0 - + COUNT 10
```

### Последние 30 точек агрегата 20 секунд

```redis
TS.REVRANGE ts:avg:20s:0 - + COUNT 30
TS.REVRANGE ts:min:20s:0 - + COUNT 30
TS.REVRANGE ts:max:20s:0 - + COUNT 30
TS.REVRANGE ts:cnt:20s:0 - + COUNT 30
```
<img width="1851" height="905" alt="изображение" src="https://github.com/user-attachments/assets/831a155f-3971-4138-bcec-d4d22e61cb8d" />

### Последние 30 точек агрегата 1 минута

```redis
TS.REVRANGE ts:avg:1m:0 - + COUNT 30
TS.REVRANGE ts:cnt:1m:0 - + COUNT 30
```
## Важные ключи в Redis 

### Stream

- `sensor_data` — входные события

### TimeSeries

Для каждого `<tag>`:

- **raw:** `ts:raw:<tag>`
- **агрегаты 20s:**
  - `ts:avg:20s:<tag>`
  - `ts:min:20s:<tag>`
  - `ts:max:20s:<tag>`
  - `ts:cnt:20s:<tag>`
- **агрегаты 1m:**
  - `ts:avg:1m:<tag>`
  - `ts:min:1m:<tag>`
  - `ts:max:1m:<tag>`
  - `ts:cnt:1m:<tag>`
- *(опционально)* счётчик "плохого времени":
  - `ts:bad_time:20s:<tag>`

### Hash

- `light:current_values`
  - `field = <tag>`
  - `value = строка вида: value,quality,event_time,stream_id,time_status,delta_ms`

### Marker

- `rts:last_ms` — последний обработанный arrival timestamp (ms)

---

## Consumer group: как читаем и почему это "at-least-once"

Worker читает Stream через `XREADGROUP`:

1. сначала делает проход по pending (PEL) — "добрать незакрытое после рестарта"
2. затем переключается на `>` — "только новые сообщения"

Подтверждение обработки делается `XACK` после записи в RTS/Hash.

Это даёт **at-least-once**: при падении worker возможна повторная обработка.

Raw запись частично защищена от дублей:

- `TS.ADD ... ON_DUPLICATE LAST` (точка с тем же timestamp перезапишется)

---

## Настройки (env переменные)

Все настройки читаются из переменных окружения (через `getenv` в коде).

Задавать можно:

- в `docker-compose.yaml` → `services.dev-window.environment`
- или внутри контейнера перед запуском: `export ...`

### Stream trimming (XTRIM по времени)

- `STREAM_RETENTION_MS` — сколько хранить Stream по времени (пример: 20 минут = `1200000`)
- `STREAM_TRIM_EVERY_MS` — как часто триммить (пример: `1000`)

### Retention для TS

- `RAW_RETENTION_MS` — raw retention (по умолчанию ~10 минут)
- `DS_20S_RETENTION_MS` — агрегаты 20s (по умолчанию ~1 час)
- `DS_1M_RETENTION_MS` — агрегаты 1m (по умолчанию ~1 день)

### Worker чтение

- `READ_COUNT` — сколько сообщений читать за раз
- `READ_BLOCK_MS` — сколько ждать новые сообщения (`BLOCK`)

### bad time threshold

- `BAD_TIME_THRESHOLD_MS` — порог рассинхрона времени (ms)

---

## Troubleshooting

### RedisInsight не открывается

Проверь, что контейнер поднялся и порт проброшен:

```bash
docker compose ps
docker compose logs -n 50 redisinsight
```

Проверка на сервере:

```bash
curl -I http://127.0.0.1:5540
```

### Worker не читает Stream

Проверить group:

```bash
redis-cli -h redis XINFO GROUPS sensor_data
redis-cli -h redis XPENDING sensor_data rts_group
```

Проверить логи:

```bash
tail -n 200 /tmp/worker.log
```

### Нет ключей `ts:*`

Проверь, что модуль timeseries загружен:

```bash
redis-cli -h redis MODULE LIST
```

---

## Структура проекта

- `src/loader.cpp` — генератор событий в Stream
- `src/worker.cpp` — обработчик Stream → RTS/Hash/Marker
- `script/run.sh` — перезапуск loader/worker, логи в `/tmp`
- `docker-compose.yaml` — Redis Stack Server + RedisInsight + dev-container
- `docker/Dockerfile.dev.ubuntu` — dev-образ для сборки
- `build/` — директория сборки (в контейнере монтируется )
