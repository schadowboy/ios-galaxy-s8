for s in 8 9 10; do
  echo "== session $s"
  grep -h -E '^TOTAL|^pass [0-9]+:' /tmp/stream_s$s.txt | cut -c1-250
done
