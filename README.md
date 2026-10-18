# beliefdb

Хранилище графа обоснованных утверждений. Как SQLite — реляционная база, так beliefdb — эпистемическая: у каждого утверждения известно, откуда оно взялось (цепочка до первичных источников), насколько надёжно (с явным методом оценки), что из него следует и с чем спорит. Главная операция — `why(X)`.

Полное ТЗ: [docs/spec.md](docs/spec.md). Решения по реализации и что взято из TMS/Graphiti: [docs/design-notes.md](docs/design-notes.md).

## Состояние

| Этап | Что | Статус |
|---|---|---|
| Ф1 | ядро: узлы, рёбра, инварианты, observe / assert / derive / link / why | готов |
| Ф1.5 | структурный scope (пересечение, вложенность, наследование), типизированный value, реестр предикатов | готов |
| Ф2 | калорийность: множество источников, nutrition(), разбавленность | готов |
| Ф3 | retract / supersede, каскадный stale, ленивый пересчёт (recompute), what_if | готов |
| Ф4 | противоречия (явные и найденные по типизированным value), кандидаты на refines, вопросы | готов |
| Ф5 | ворота записи: батчи с квотами, гипотезы с подтверждением и истечением; дедуп, поиск, эмбеддинги; HTTP-сервис `beliefd` | готов |
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
belief nutrition $D
belief predicates
```

```
nutrition: 1 source, 1 head, weight 0.80, concentration 1.00, depth 2, dilution 2.00, observed 2026-10-10..2026-10-10
c_01M52… claim 0.95 min  Суточная доза 800 мг
└─ derives_from [dose_from_prescription] c_01M52… claim 0.95 stated  Ибупрофен 400 мг 2 раза в день
   └─ supports s_01M52… source  document: visit:2026-10-10
```

Scope производного утверждения вычисляется как пересечение scope оснований; флагами его можно только сузить. Значения типизированы (`bool`, `number` как интервал с единицей, `enum`, `ref`, `text`), и предикат держит один тип: `weight` в килограммах нельзя потом записать в метрах, ошибка `type_mismatch`.

Калорийность — это множество первичных источников в транзитивном замыкании обоснования: три пересказа одного документа остаются одним источником, а цепочка выводов не добавляет ни одного. `nutrition` показывает число источников, число независимых «голов» (по `attribution`), взвешенную сумму по виду источника, концентрацию (долю веса самого крупного источника) и разбавленность (глубина к числу источников).

Источник — это всегда свидетельство из мира: `measurement`, `document`, `human_statement` или `llm_output`. Вида «вывод» нет. Чужой вывод, чьих оснований мы не видим («наш анализ показывает X»), записывается как `document`/`human_statement` с `--directness secondary`: вес вида умножается на 0.5, а в `nutrition` это видно как `N secondary`. Опора на утверждение другого домена — это ребро `derives_from`/`supports` на тот узел, а не новый источник: `observe` отвергает источник, в `uri` или тексте которого встречается id узла базы (`bridge_source`). Перенос накопленного знания из заметок и переписки — `human_statement` с `--attribution` того, кто это знал, а не особый вид.

Ничего не удаляется. `belief retract ID --reason R` помечает узел (источник, утверждение или правило) и ставит флаг `STALE` на всё, что от него зависит; старые значения продолжают отдаваться с флагом. `belief supersede OLD --reason R <параметры observe/assert/derive>` записывает исправление, связывает его ребром `replaces` с прежним узлом и так же помечает следствия; в цепочке `replaces` активен ровно один узел. `belief stale` показывает помеченные утверждения, мелкие первыми, с причиной каждой пометки. `belief recompute ID` пересчитывает одно утверждение по явной команде: ребро на вытесненное основание переводится на его замену, ретрактнутые основания отбрасываются, метод `min` считает значение сам, остальные методы требуют `--credence`. `belief what-if ID --credence X` показывает, что изменится ниже по графу, ничего не записывая. `why` на вытесненном или ретрактнутом узле начинается со строки `NOTICE`, а выбитая опора в дереве помечена `✗`.

Противоречия не разрешаются автоматически, а всплывают. `belief conflicts` находит пары живых утверждений с одним subject и predicate, пересекающимся scope и несовместимыми типизированными значениями, добавляет явные рёбра `contradicts` и ранжирует по числу зависимых: сначала то, на чём стоит больше всего. Текст никогда не сравнивается. Если scope одного строго вложен в scope другого, пара попадает не в конфликты, а в `belief refinements` с предложением поставить `refines`. После `assert`/`derive` CLI предупреждает в stderr, если новое утверждение с чем-то спорит. Состояние разбора хранится: каждая найденная пара помнит, когда всплыла и сколько раз; `belief dismiss A B --reason` говорит «не конфликт», `belief resolve A B --reason` «разобрано», и пара уходит из выдачи, пока одна из сторон не изменится (value, scope или статус), тогда решение истекает и пара всплывает снова с прежней причиной. Пары, которые перестали находиться, помечаются vanished; явное ребро `contradicts` делает пару promoted. `belief stats` показывает счётчики и точность детектора. Вопрос — первоклассный объект: `belief ask --text … --criterion …` требует критерий закрытия, `progress` добавляет датированные заметки, `close --resolution` закрывает; `belief questions` показывает открытые, а конфликты показывают вопросы, заведённые про эту пару.

Машинный вывод — это гипотеза, а не утверждение. `belief hypothesize --text … --from C1,C2 --rule R` записывает узел `h_` с теми же основаниями, что и `derive`, но он ничего не обосновывает: его нельзя указать в `--support` или `--from`, а credence у него — минимум по основаниям как потолок, не оценка. Источников он не добавляет (калорийность — объединение оснований), в детектор противоречий не попадает. `belief confirm HID --support S --credence X --method M` превращает гипотезу в утверждение на основании свидетельства: новое `c_` наследует `derives_from` и получает `supports`, гипотеза становится superseded с пометкой «confirmed». Отклонение — обычный `retract HID --reason R`. Не рассмотренная за 30 дней гипотеза ретрактится сама при следующем обращении (`expired`). `belief hypotheses` показывает очередь на рассмотрение; когда в домене накопилось 20 нерассмотренных, новые не принимаются, пока старые не разобраны.

Запись идёт батчами. `belief batch begin --purpose "…"` открывает батч для актора с заявленной целью и печатает id; дальше `export BELIEF_BATCH=ID` (или `--batch ID`), и каждая запись считается в его счёт и помечается им в журнале ревизий. Квоты на батч: 20 утверждений и 5 гипотез; переполненный или простоявший 30 минут батч закрывается сам, и пока актор не откроет новый с новой целью, его записи отклоняются (`no_batch`). Суточный лимит на пару (домен, актор): 50 утверждений и 10 гипотез, через все батчи, иначе упёршийся в квоту агент просто открыл бы следующий (`quota_exceeded`). `belief batch show ID` показывает, что в нём записано, `belief batch reject ID --reason R` ретрактит всё созданное в батче: это единица ревью. Цифры задаются через `Store::set_policy`; `require_batch` запрещает запись вне батча вовсе (для агентов). В `beliefd.json` актору можно задать свои `daily_claims`/`daily_hypotheses` (например, контроллеру, который разбирает архив из сотен постов), глобальная политика для остальных не меняется.

Дубли не записываются молча. Если уже есть живое утверждение (или нерассмотренная гипотеза) с тем же subject и predicate, пересекающимся scope и совместимым типизированным значением, `assert`/`derive`/`hypothesize` отвечают `duplicate_claim` и называют его: добавьте своё свидетельство к нему (`link SRC ID supports`), подтвердите гипотезу, либо укажите `--despite ID` и свяжите новое утверждение ребром `refines` или `contradicts`. Несовместимое значение — не дубль, а противоречие, его найдёт детектор. Текстовые значения несравнимы, их никто не склеивает: `belief similar --text … --subject … --predicate …` показывает кандидатов для человека, структурные совпадения первыми, затем полнотекстовые (FTS5, триграммы, поэтому русская морфология не мешает) и, если настроены эмбеддинги, ближайшие по косинусу. `belief search QUERY` ищет по всем утверждениям и гипотезам любого статуса. После записи CLI печатает в stderr `note: similar …`, если рядом что-то есть.

Эмбеддинги — только генератор кандидатов, решение «то же или нет» принимается по структурным полям. Провайдер задаётся переменной `BELIEF_EMBEDDINGS` (URL OpenAI-совместимого `/v1/embeddings`, у нас bge-m3 через llama-server на casablanca:12000), умолчания нет, наружу ничего не ходит. Вектор пишется при записи утверждения рядом с `model_id` и размерностью; смена модели делает старые векторы невидимыми, `belief reindex-embeddings` считает недостающие. Недоступный провайдер не ломает запись и не задерживает её дольше таймаута: поиск деградирует до FTS и говорит об этом в `mode`/`notice`, следующие попытки ждут минуту.

`belief why ID --json` отдаёт то же дерево в JSON; `belief show ID` — узел с рёбрами; `belief stats` — счётчики. Ошибки уходят в stderr как `error[code]: message`, код возврата 2.

## Сервис (`beliefd`)

`beliefd --config beliefd.json` поднимает HTTP-сервис с теми же вызовами в JSON, для министерств fnc-hq. Пример конфига и user-юнит systemd — в `deploy/`. Файл не содержит секретов: токены акторов берутся из окружения как `BELIEFD_TOKEN_<NAME>` (`health` → `BELIEFD_TOKEN_HEALTH`), актор без токена отключён с предупреждением при старте. Запрос подписывается `Authorization: Bearer <token>`; актор пишет только в свои домены (`domains` в конфиге), человек (`"human": true`) видит и меняет всё и не нуждается в батче.

```sh
export BELIEFD_TOKEN_HEALTH=…
B=$(curl -s -X POST -H "Authorization: Bearer $BELIEFD_TOKEN_HEALTH" localhost:7420/batch/begin \
      -d '{"purpose":"разбираю визит к врачу"}' | jq -r .id)
curl -s -X POST -H "Authorization: Bearer $BELIEFD_TOKEN_HEALTH" -H "X-Belief-Batch: $B" localhost:7420/assert \
  -d '{"text":"ибупрофен 400 мг","subject":"me","predicate":"ibuprofen_dose","value":{"type":"number","value":400,"unit":"мг"},
       "support":["s_…"],"credence":0.8,"method":"stated","scope":{"valid_from":"2026-10-10"},"domain":"health"}'
# → {"id":"c_…","conflicts":[…],"similar":[…],"search_mode":"fts+embeddings"}
```

Маршруты повторяют CLI: `POST /observe /assert /derive /hypothesize /confirm /link /retract /supersede /recompute /rule /predicate /ask /progress /close /dismiss /resolve /similar /batch/begin /batch/end /batch/reject /reindex-embeddings`; `GET /health /whoami /node/ID /why/ID /nutrition/ID /what-if/ID?credence= /stale /conflicts /refinements /questions /question/ID /hypotheses /hypothesis/ID /search?q= /predicates /rule/NAME /batches /batch/ID`. Ошибки — `{"error": code, "message": …}` с кодами 400 (ввод), 401 (токен), 403 (чужой домен, приватный узел), 404, 409 (`no_batch`, `duplicate_claim`, `already_replaced`), 429 (квоты).

Батч передаётся заголовком `X-Belief-Batch` в каждой записи; при `require_batch` в политике агент без него получает 409. `sensitivity: private` делает узел невидимым для акторов чужих доменов: `GET /node` отвечает 403, в `why` поддерево заменяется заглушкой `(private to health)`, из поиска, списков и сторон конфликтов такие узлы вырезаются; обоснование через `domain`-узлы чужого домена разрешено (это ребро, не копия). Тихие часы для GPU: `embeddings.quiet_hours: "23:00-08:00"` по локальному времени сервера, в это окно сервис не обращается к провайдеру эмбеддингов вовсе, поиск работает по FTS, `GET /health` показывает `quiet_now`.

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
