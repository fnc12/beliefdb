# beliefdb

Хранилище графа обоснованных утверждений. Как SQLite — реляционная база, так beliefdb — эпистемическая: у каждого утверждения известно, откуда оно взялось (цепочка до первичных источников), насколько надёжно (с явным методом оценки), что из него следует и с чем спорит. Главная операция — `why(X)`.

Полное ТЗ: [docs/spec.md](docs/spec.md). Решения по реализации и что взято из TMS/Graphiti: [docs/design-notes.md](docs/design-notes.md).

## Состояние

| Этап | Что | Статус |
|---|---|---|
| Ф1 | ядро: узлы, рёбра, инварианты, observe / assert / derive / link / why | готов |
| Ф1.5 | структурный scope (пересечение, вложенность, наследование), типизированный value, реестр предикатов | готов |
| Ф2 | калорийность: множество источников, nutrition(), разбавленность | — |
| Ф3 | retract / supersede, каскадный stale, ленивый пересчёт, what_if | — |
| Ф4 | противоречия и вопросы | — |
| Ф5 | HTTP API, ворота записи, дедуп через FTS/эмбеддинги, квоты, гипотезы | — |
| Ф6 | калибровка: predictions, Brier, orphans | — |

## Сборка

Требования: CMake ≥ 3.24, компилятор с C++20 (gcc 11, clang 14, Apple clang 17 проверены), SQLite ≥ 3.35 с FTS5 и JSON1.

Зависимости (nlohmann/json, sqlite_orm, doctest) подтягиваются через FetchContent, но сначала ищутся в локальной папке `~/deps` (см. [cmake/deps.cmake](cmake/deps.cmake)): клонирование nlohmann/json с GitHub занимает десятки минут, а чистая сборка делается часто.

```sh
# один раз на машину
mkdir -p ~/deps && cd ~/deps
curl -L -o json.tar.xz https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz && tar xf json.tar.xz && rm json.tar.xz
git clone --branch dev https://github.com/fnc12/sqlite_orm.git
git clone --depth 1 --branch v2.4.12 https://github.com/doctest/doctest.git

# сборка
cmake -S . -B build -G Ninja
cmake --build build
./build/beliefdb_tests
```

Другое место для зависимостей: `-DBELIEFDB_DEPS_DIR=/path`.

## CLI (`belief`)

```sh
export BELIEF_DB=~/beliefdb.sqlite            # или --db PATH

S=$(belief observe --kind document --uri "visit:2026-10-10" --attribution "врач Иванов" \
      --content "Врач: ибупрофен 400 мг 2 раза в день, 5 дней" --observed-at 2026-10-10 \
      --domain health --sensitivity private)
R=$(belief rule --name dose_from_prescription --description "доза следует из назначения")
C=$(belief assert --text "Ибупрофен 400 мг 2 раза в день" --support $S --credence 0.95 --method stated \
      --subject me --predicate ibuprofen_dose --value-type number --value 400 --unit мг \
      --valid-from 2026-10-10 --valid-to 2026-10-15 --cond patient=me --domain health)
D=$(belief derive --text "Суточная доза 800 мг" --from $C --rule dose_from_prescription \
      --predicate ibuprofen_daily --value-type number --value 800 --unit мг)
belief why $D
belief predicates
```

```
c_01M52… claim 0.95 min  Суточная доза 800 мг
└─ derives_from [dose_from_prescription] c_01M52… claim 0.95 stated  Ибупрофен 400 мг 2 раза в день
   └─ supports s_01M52… source  document: visit:2026-10-10
```

Scope производного утверждения вычисляется как пересечение scope оснований; флагами его можно только сузить. Значения типизированы (`bool`, `number` как интервал с единицей, `enum`, `ref`, `text`), и предикат держит один тип: `weight` в килограммах нельзя потом записать в метрах, ошибка `type_mismatch`.

`belief why ID --json` отдаёт то же дерево в JSON; `belief show ID` — узел с рёбрами; `belief stats` — счётчики. Ошибки уходят в stderr как `error[code]: message`, код возврата 2.

## Библиотека

```cpp
#include <beliefdb/store.hpp>
auto db = beliefdb::Store::open("beliefdb.sqlite", "agent:health");
auto src = db.observe({.kind = "document", .uri = "...", .content = "...", .observed_at = "2026-10-10"});
auto id  = db.assert_claim({.text = "...", .support = {src}, .credence = 0.8,
                            .method = beliefdb::CredenceMethod::stated, .scope = {.note = "..."}});
auto why = db.why(id);  // WhyNode: дерево до источников, с credence и методом на каждом уровне
```

Все ошибки — `beliefdb::Error` с полем `code` (`no_support`, `cycle`, `bad_credence`, `empty_scope`, …).
