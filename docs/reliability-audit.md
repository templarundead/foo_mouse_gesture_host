# A–E reliability audit

Дата проверки: 2026-10-26. Ветка: `arena/ee02d675-foo-mouse-gesture-host`.
База: `dea8d046e645361610bd195eaa00427bdd12156e` (`main`). Pull request: #22.

Исходная версия компонента — `1.3.1` (`src/component_version.h:4`), а GitHub
Latest на момент аудита — `v1.1.4`. Это не скрывается и не исправляется фиктивным
релизом: новый выпуск возможен только через тег `v1.3.1`, который workflow
сверит с исходником. Инструкция и ручная подпись описаны в `README.md:326-363`.

## Inventory и статус

### A / P0 — supply chain и выпуск: выполнено в коде и CI

| Проверка | Реализация |
|---|---|
| Независимые SHA-256 официального SDK и mirror; распаковка только после проверки | `tools/fetch-sdk.ps1:29-47`, `tools/fetch-sdk.ps1:59-100` |
| Действия закреплены полными SHA, а не mutable tags | `.github/workflows/build.yml:32-40`, `:73-79`, `:123-128`, `:143-147`, `:171-179` |
| Общий `contents: read`; `contents: write` только у release-job | `.github/workflows/build.yml:10-11`, `:162-167` |
| Точное равенство тега и `MGH_COMPONENT_VERSION` при build и release | `.github/workflows/build.yml:86-99`, `:188-194` |
| Ровно два versioned x64/x86 пакета, один DLL в ZIP, PE `0x8664`/`0x14C` | `.github/workflows/build.yml:100-120`, `:196-240` |
| Публикация `SHA256SUMS.txt`; необязательная ручная Authenticode-подпись | `.github/workflows/build.yml:242-256`, `README.md:356-363` |
| Незакреплённый choco исключён из hot path | `tools/fetch-sdk.ps1:77-88` |

### B / P1 — hook thread и сессия: реализовано; нужен ручной Windows runtime

| Проверка | Реализация |
|---|---|
| Выделенный `_beginthreadex` worker с message pump и message-only sink | `src/gesture_engine.cpp:96-149`, `:191-243` |
| Минимальный numeric callback; bounded ring 256, drop counter, старые move вытесняются первыми | `src/gesture_engine.cpp:531-561`, `src/gesture_session.h:101-153` |
| Recognizer и строки в worker вне callback; `SendInput`, команды и SDK console только на main thread | `src/gesture_engine.cpp` |
| Own-event `dwExtraInfo` marker вместо injected + 500 ms | `src/gesture_engine.cpp:19`, `:451-468`, `:531-536` |
| Replay по сохранённым координатам после проверки главного окна и физического состояния ПКМ | `src/gesture_engine.cpp:431-472` |
| Health timer, reinstall, exponential backoff и bounded logs | `src/gesture_engine.cpp:474-528`, `src/gesture_session.cpp:248-272` |
| Reconciliation через `GetAsyncKeyState`; явные idle/right-down/rocker/wheel состояния | `src/gesture_engine.cpp:474-490`, `src/gesture_session.cpp:17-234` |
| COW snapshot при Apply без ожидания main thread со стороны worker | `src/gesture_engine.cpp:177-188`, `:575-585` |

Callback не вызывает menu services, `SendInput`, модальные диалоги или SDK console.
Потерянные события подсчитываются; новое критическое событие при полном ring
пропускается к системе (fail open), а его session state откатывается. Потерянный
move всё равно помечает drag. Это overload policy, а не утверждение о нулевых
потерях при любой нагрузке.

### C / P1–P2 — конфигурация и идентичность команд: выполнено

| Проверка | Реализация |
|---|---|
| SDK v2 tree traversal, command/subcommand GUID, structural node path | `src/command_catalog.cpp:51-102`, `:138-185` |
| Fail-closed GUID/structural/legacy resolution без leaf fallback | `src/command_identity.h:39-105`, `src/command_catalog.cpp:233-264` |
| Schema v2 из семи полей, строгие group/place/GUID/node/UTF-8 проверки | `src/gesture_config_serialization.cpp:167-214`, `:231-324` |
| Migration без потери label/binding; полные duplicates удаляются, первая строка wins | `src/gesture_config_serialization.cpp:254-324` |
| Uninitialized отличается от intentionally empty; last-valid backup | `src/gesture_config.cpp:68-119`, `:121-153` |
| `[unavailable]`/`[conflict]` в существующем dropdown; Add/Update сохраняет identity | `src/preferences_page.cpp:1810-1850`, `:1880-1950` |

Существующие `cfg_var` GUID и четыре defaults `L/R/U/D` не изменены. Import/Export
не добавлялся: это необязательная часть scope, отдельный формат/диалоги не стали
расширением этого reliability PR.

### D / P1 — portable tests: выполнено

- Все 12 tokens, malformed/empty/Unicode: `tests/logic_tests.cpp:47-107`.
- Candidate order и 136 deterministic trajectories: `tests/logic_tests.cpp:108-224`.
- Config roundtrip/migration/corruption/duplicates: `tests/logic_tests.cpp:225-322`.
- GUID rename/ambiguity, structural mismatch, legacy duplicates, invalid group/place,
  identity copy: `tests/logic_tests.cpp:323-406`.
- Differential exact/prefix/wheel index tests: `tests/logic_tests.cpp:407-495`.
- Tap/drag/reconcile, оба rocker, fractional/capped wheel, queue overflow,
  fake-clock health/backoff: `tests/logic_tests.cpp:496-668`.
- Отдельные Logic tests x64/x86 jobs: `.github/workflows/build.yml:130-160`,
  `tests/logic_tests.vcxproj`.

### E / P2 — indexes: выполнено без оптимизации recognizer math

Immutable snapshot строится при конфигурации/reload и содержит exact/prefix/wheel
индексы (`src/binding_index.h:23-60`, `src/binding_index.cpp:1-77`). Hot path
получает `span` номеров строк в исходном порядке, не копируя bindings. Main/worker
разделяют COW snapshot. Benchmark 100/1000 bindings находится в
`tests/logic_tests.cpp:670-706` и запускается обеими CI test jobs.

Recognizer math, repeated-direction collapsing, Preferences layout и wheel UX
не менялись. p99 callback <50 µs и zero-allocation остаются целями, а не
заявленными измерениями.

## Коммиты

| Commit | Содержание diff |
|---|---|
| `30eec09` `ci: pin workflow actions and mirror SDK` | Полные Action SHA, официальный SDK + отдельно проверяемый mirror, удаление choco. |
| `1136f0a` `ci: enforce SDK and release integrity` | Официальный SDK hash, tag/version, PE/package/checksum gates, scoped release permission, signing docs. |
| `9e49ef4` `test: add portable gesture logic tests` | Первый UI-independent test target и x64/x86 CI test jobs. |
| `85e9d85` `fix: declare SDK dependencies explicitly` | Явные Win32 declarations после потери transitive includes в предыдущем split. |
| `0f32ab1` `feat: version and validate binding configuration` | Schema v2, migration, diagnostics, dedup, initialized/empty и backup. |
| `198f937` `perf: index immutable binding snapshots` | Exact/prefix indexes, COW snapshot, differential tests и informational benchmark. |
| `7089728` `fix: resolve commands by stable identity` | SDK v2 catalog, GUID/structural resolver, fail-closed execution и Preferences statuses. |
| `494c69b` `test: cover mouse session and hook health` | Numeric session, bounded ring, fake-clock health и portable tests. |
| `b0cc230` `fix: isolate low level mouse hook thread` | Worker message pump, main-thread command queue, own marker, replay, health/reinstall. |
| `1315b49` `docs: document hook thread boundaries` | README с фактическими thread boundaries и ручным Windows checklist. |

## Фактические команды и результаты

Локальный Linux компилятор не заменяет MSVC/Windows runtime; он проверяет portable
часть со строгими warnings:

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc \
  tests/logic_tests.cpp src/gesture_tokens.cpp src/gesture_recognizer.cpp \
  src/gesture_config_serialization.cpp src/binding_index.cpp \
  src/gesture_session.cpp -o build/logic-tests/linux/logic_tests
build/logic-tests/linux/logic_tests --benchmark
```

Результат последнего запуска:

```text
Logic tests passed: 12 tokens, candidate order, 136 deterministic trajectories, config, index, session and health
Binding benchmark 100 rows: indexed 40.559 ns/lookup, linear 363.495 ns/lookup (informational)
Binding benchmark 1000 rows: indexed 127.31 ns/lookup, linear 3447.13 ns/lookup (informational)
Benchmark sink: 246680
```

MSVC Release x64/x86 и Logic tests x64/x86 выполнялись в GitHub Actions.
Ключевые runs: `40375873551`, `40376525560`, `40377138749`, `40377850239`,
`40378417106`, `40379118665`, `40379943401`, `40380581276`, `40380999749` —
все четыре required jobs success; release creation вне тега корректно skipped.
Промежуточный run `40376936329` для `9e49ef4` выявил missing Win32 declarations;
это исправлено отдельным `85e9d85`, а не скрыто.

`git diff --check` и разбор обоих project XML также выполнялись перед commit.
Новых runtime dependencies нет.

## Behavioral observations

- Candidate order остаётся exact 8-way → expanded diagonals → 4-way fallback;
  первая подходящая привязка сохраняет приоритет. Duplicate gesture rows при
  исполнении сохраняют исходный row order и флаг first-only.
- `R R`/`U U`/`DR DR`, `WU WU`, fractional wheel accumulation, оба rocker и
  ordinary tap/drag policy покрыты без изменения recognizer math.
- GUID identity переживает display rename; structural identity дополнительно
  требует сохранённый полный label. Ноль совпадений = unavailable, несколько =
  conflict, исполнение fail closed с диагностикой.
- Полностью повреждённая таблица не вызывает неожиданный возврат defaults:
  сначала пробуется last-valid backup, затем используются только валидные строки.

## Риски, ограничения и follow-ups

1. В sandbox не запускался интерактивный foobar2000. Нужны ручные Windows 10/11
   проверки реального hook/replay и меню сторонних компонентов.
2. Health/backoff проверен fake clock, но настоящий Windows hook timeout и
   последующий reinstall не воспроизводились интеграционно.
3. Не измерялись callback p99, allocations внутри Win32 или latency под modal
   dialog. Приведённые ns относятся только к pure lookup benchmark.
4. Latest остаётся `v1.1.4`, пока maintainer сознательно не выпустит `v1.3.1`.
5. Import/Export и автоматическая код-подпись не входят в этот PR.

## Reviewer instructions

1. Проверить diff инвариантов: cfg GUID/defaults, recognizer и Preferences layout
   не должны измениться; wheel review делается по session differential tests.
2. Выполнить `tools\fetch-sdk.ps1`, затем Release x64 и Win32 команды из README;
   отдельно собрать и запустить Logic tests для обеих архитектур.
3. В foobar2000 проверить немедленный обычный context menu, отсутствие меню после
   drag, `↓ → ↑ ←` на sharp/rounded/almost-circular траекториях, диагонали,
   `R+L`/`L+R`, `WU`/`WD`/`WU WU` и fractional deltas.
4. Проверить gestures только над главным окном, Disable, Apply во время работы,
   unavailable/conflict после изменения меню и legacy migration с backup.
5. Убедиться в Console в диагностике dropped events/commands и fail-closed
   execution. Для release review отдельно проверить два пакета, PE machines и
   `SHA256SUMS.txt`; checksum не считать Authenticode-подписью.
