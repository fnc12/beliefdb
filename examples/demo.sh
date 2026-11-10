#!/bin/sh
# Rebuilds the README's tour in a fresh database and prints the justification tree.
#   examples/demo.sh [path/to/belief] [demo.sqlite]
set -e
B=${1:-./build/belief}; DB=${2:-demo.sqlite}
rm -f "$DB" "$DB-wal" "$DB-shm"
S1=$($B --db "$DB" observe --kind document --uri "paper:doi/10.1000/xyz" --attribution "Smith et al. 2024" --observed-at 2024-05-02 --domain sleep \
      --content "Participants who slept under 6 hours showed a 30% drop in working-memory scores")
S2=$($B --db "$DB" observe --kind human_statement --uri "chat:dr-lee/2026-11-01" --attribution "Dr. Lee" --observed-at 2026-11-01 --domain sleep \
      --content "Less than six hours of sleep costs you a third of your working memory the next day")
C=$($B --db "$DB" assert --text "Sleeping under 6 hours lowers working memory by about 30%" --support "$S1,$S2" --credence 0.8 --method stated \
      --subject sleep_deprivation --predicate working_memory_drop --value-type number --value 30 --unit % --domain sleep)
$B --db "$DB" rule --name exam_risk --description "a memory drop before an exam lowers the expected score" >/dev/null
D=$($B --db "$DB" derive --text "Pulling an all-nighter before the exam will cost points" --from "$C" --rule exam_risk --domain sleep)
S3=$($B --db "$DB" observe --kind document --uri "paper:doi/10.1000/abc" --attribution "Jones 2025" --observed-at 2025-03-10 --domain sleep \
      --content "A replication found a 10% drop, not 30%")
$B --db "$DB" assert --text "The working-memory drop after short sleep is closer to 10%" --support "$S3" --credence 0.65 --method stated \
      --subject sleep_deprivation --predicate working_memory_drop --value-type number --value 10 --unit % --domain sleep 2>/dev/null >/dev/null || true
echo "== why $D"; $B --db "$DB" why "$D"
echo; echo "== conflicts"; $B --db "$DB" conflicts
echo; echo "== after retracting the first paper"; $B --db "$DB" retract "$S1" --reason "paper retracted by the journal" >/dev/null; $B --db "$DB" why "$D"
