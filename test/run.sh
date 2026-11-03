#!/bin/bash
# Fetches live fixtures (never committed: NIED content) and runs the host tests.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/test; mkdir -p "$OUT"
B=http://www.kmoni.bosai.go.jp
get() { curl -sS -m 8 --retry 5 --retry-connrefused --retry-all-errors -o "$OUT/$2" -f "$B/$1"; }
T=$(curl -sS -m 8 --retry 5 --retry-all-errors "$B/webservice/server/pros/latest.json" |
    python3 -c 'import json,sys,datetime as d; print((d.datetime.strptime(json.load(sys.stdin)["latest_time"],"%Y/%m/%d %H:%M:%S")-d.timedelta(seconds=2)).strftime("%Y%m%d%H%M%S"))')
get data/map_img/CommonImg/base_map_w.gif base.gif
get data/map_img/ScaleImg/nied_jma_s_w_scale.gif scale.gif
get "data/map_img/RealTimeImg/jma_s/${T:0:8}/$T.jma_s.gif" stations.gif
# A real EEW (2024 Noto) so the optional layers carry data.
E=20240101161100
get "data/map_img/EstShindoImg/eew/${E:0:8}/$E.eew.gif" est.gif
get "data/map_img/PSWaveImg/eew/${E:0:8}/$E.eew.gif" ps.gif
# JMA: the list, the map outlines, and the largest and a mid-sized report in the list right now.
JB=https://www.jma.go.jp/bosai
curl -sS -m 25 --retry 3 --compressed -o "$OUT/quake_list.json" -f "$JB/quake/data/list.json"
curl -sS -m 25 --retry 3 --compressed -o "$OUT/quake_geo.json" -f "$JB/common/const/geojson/earthquake.json"
python3 -I - "$OUT/quake_list.json" > "$OUT/quake_picks.txt" <<'PY'
import json, sys
L = json.load(open(sys.argv[1]))
c = [e for e in L if e.get("ttl") == "震源・震度情報" and e.get("json")]
c.sort(key=lambda e: -len(json.dumps(e.get("int", ""))))
print(c[0]["json"]); print(c[len(c) // 2]["json"])
PY
i=0
while read -r f; do
  i=$((i + 1))
  curl -sS -m 25 --retry 3 --compressed -o "$OUT/quake_detail$i.json" -f "$JB/quake/data/$f"
done < "$OUT/quake_picks.txt"
# The long-term database: a search, and its biggest report (the 2011 Tohoku earthquake).
DB=https://www.data.jma.go.jp/eqdb/data/shindo/api/
curl -sS -m 40 --retry 3 -o "$OUT/quake_db_search.json" -f -X POST "$DB" -d 'mode=search&dateTimeF[]=2011-03-01&dateTimeF[]=00:00&dateTimeT[]=2011-03-31&dateTimeT[]=23:59&mag[]=0.0&mag[]=9.9&dep[]=0&dep[]=999&epi[]=99&pref[]=99&city[]=99&station[]=99&obsInt=1&maxInt=A&additionalC=true&Sort=S0&Comp=C0&seisCount=false&observed=false'
curl -sS -m 40 --retry 3 -o "$OUT/quake_db_event.json" -f -X POST "$DB" -d 'mode=event&id=20110311144618'
curl -sS -m 40 --retry 3 -o "$OUT/quake_db_old.json" -f -X POST "$DB" -d 'mode=search&dateTimeF[]=1919-01-01&dateTimeF[]=00:00&dateTimeT[]=1925-12-31&dateTimeT[]=23:59&mag[]=0.0&mag[]=9.9&dep[]=0&dep[]=999&epi[]=99&pref[]=99&city[]=99&station[]=99&obsInt=1&maxInt=3&additionalC=true&Sort=S0&Comp=C0&seisCount=false&observed=false'
for n in base scale stations est ps; do sips -s format bmp "$OUT/$n.gif" --out "$OUT/$n.bmp" >/dev/null; done
clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" -o "$OUT/host_test" test/host_test.cpp
"$OUT/host_test" "$OUT"
