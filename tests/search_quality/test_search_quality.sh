#!/usr/bin/env bash
#
# Search Quality Test Framework
#
# Queries SearXNG directly (no running DAWN needed) and saves raw JSON
# responses for comparison analysis. Simulates DAWN's news supplement
# merge for categories that don't support time_range.
#
# Usage:
#   bash test_search_quality.sh <label> <output_dir>
#   bash test_search_quality.sh before /tmp/search_before
#   bash test_search_quality.sh after /tmp/search_after

set -uo pipefail

LABEL="${1:?Usage: $0 <label> <output_dir>}"
OUTPUT_DIR="${2:?Usage: $0 <label> <output_dir>}"
SEARXNG_URL="${SEARXNG_URL:-http://localhost:8384}"

mkdir -p "$OUTPUT_DIR"

# Categories that support time_range natively (tested empirically)
supports_time_range() {
   case "$1" in
      news|science|general|web|"") return 0 ;;
      *) return 1 ;;
   esac
}

url_encode() {
   python3 -c "import urllib.parse; print(urllib.parse.quote('$1'))"
}

count_results() {
   python3 -c "import json; d=json.load(open('$1')); print(len(d.get('results',[])))" 2>/dev/null || echo "0"
}

# Merge results from two JSON files into one (deduped by URL)
merge_results() {
   local primary="$1"
   local supplement="$2"
   local output="$3"
   python3 << PYEOF
import json

with open('$primary') as f:
    primary = json.load(f)
with open('$supplement') as f:
    supplement = json.load(f)

seen_urls = {r.get('url') for r in primary.get('results', [])}
merged = list(primary.get('results', []))
for r in supplement.get('results', []):
    if r.get('url') not in seen_urls:
        merged.append(r)
        seen_urls.add(r.get('url'))

primary['results'] = merged
with open('$output', 'w') as f:
    json.dump(primary, f)
PYEOF
}

# Test queries: "query|category|time_range|expected_keywords"
QUERIES=(
   "top news today|news|day|news,today"
   "latest breaking news|news|day|breaking,news"
   "stock market today DOW|news|day|dow,stock,market"
   "weather forecast this week|web||weather,forecast"
   "latest AI artificial intelligence news|news|week|AI,artificial,intelligence"
   "tech news this week|it|week|tech,technology"
   "Ukraine Russia war latest|news|day|ukraine,russia"
   "best python web frameworks 2026|it||python,framework"
   "how to make sourdough bread|web||sourdough,bread,recipe"
   "NBA scores today|news|day|NBA,score"
   "SpaceX launch schedule|news|week|spacex,launch"
   "climate change research|science||climate,change"
   "machine learning papers|papers||machine,learning"
   "Reddit opinions on electric cars|social|week|electric,car"
   "define ephemeral|web||ephemeral,definition"
)

echo "=== Search Quality Test: $LABEL ==="
echo "SearXNG URL: $SEARXNG_URL"
echo "Output dir:  $OUTPUT_DIR"
echo "Queries:     ${#QUERIES[@]}"
echo ""

pass=0
fail=0

for entry in "${QUERIES[@]}"; do
   IFS='|' read -r query category time_range keywords <<< "$entry"

   encoded_q=$(url_encode "$query")
   safe_name=$(echo "$query" | tr ' ' '_' | tr -cd '[:alnum:]_')
   outfile="${OUTPUT_DIR}/${safe_name}.json"

   # Determine category param for URL
   cat_param=""
   if [[ -n "$category" ]]; then
      cat_param="&categories=${category}"
   fi

   if [[ -n "$time_range" ]] && ! supports_time_range "$category"; then
      # Simulate DAWN's news supplement merge:
      # 1. Primary query WITHOUT time_range
      # 2. Supplemental news query WITH time_range
      # 3. Merge results (deduped)

      primary_url="${SEARXNG_URL}/search?q=${encoded_q}&format=json${cat_param}"
      news_url="${SEARXNG_URL}/search?q=${encoded_q}&format=json&categories=news&time_range=${time_range}"

      tmpdir=$(mktemp -d)
      primary_file="${tmpdir}/primary.json"
      news_file="${tmpdir}/news.json"

      http1=$(curl -s -o "$primary_file" -w "%{http_code}" --max-time 15 "$primary_url" 2>/dev/null || echo "000")
      http2=$(curl -s -o "$news_file" -w "%{http_code}" --max-time 15 "$news_url" 2>/dev/null || echo "000")

      if [[ "$http1" == "200" && "$http2" == "200" ]]; then
         count1=$(count_results "$primary_file")
         count2=$(count_results "$news_file")
         merge_results "$primary_file" "$news_file" "$outfile"
         merged=$(count_results "$outfile")
         printf "  %-45s  %s:%s + news:%s = merged:%s\n" "$query" "$category" "$count1" "$count2" "$merged"
         ((pass++))
      else
         printf "  %-45s  HTTP %s/%s  FAILED\n" "$query" "$http1" "$http2"
         ((fail++))
      fi

      rm -rf "$tmpdir"
   else
      # Standard query — type supports time_range or no time_range requested
      url="${SEARXNG_URL}/search?q=${encoded_q}&format=json${cat_param}"
      if [[ -n "$time_range" ]]; then
         url="${url}&time_range=${time_range}"
      fi

      http_code=$(curl -s -o "$outfile" -w "%{http_code}" --max-time 15 "$url" 2>/dev/null || echo "000")

      if [[ "$http_code" == "200" ]]; then
         count=$(count_results "$outfile")
         printf "  %-45s  HTTP %s  results: %s\n" "$query" "$http_code" "$count"
         ((pass++))
      else
         printf "  %-45s  HTTP %s  FAILED\n" "$query" "$http_code"
         ((fail++))
      fi
   fi
done

echo ""
echo "=== Summary: $pass passed, $fail failed ==="
echo "Results saved to: $OUTPUT_DIR"
