#!/usr/bin/env bash
# 冒烟测试：不需要真实 NVR，验证服务启动、healthz、参数校验与错误映射。
# 用法（容器外）: bash scripts/smoke_test.sh [port]
set -euo pipefail

PORT="${1:-8180}"
BASE="http://localhost:${PORT}"
FAIL=0

check() { # name expected_status actual_status
  if [ "$2" = "$3" ]; then
    echo "PASS  $1 (HTTP $3)"
  else
    echo "FAIL  $1 (期望 $2, 实际 $3)"
    FAIL=1
  fi
}

echo "== healthz =="
status=$(curl -sS -o /dev/null -w '%{http_code}' "${BASE}/healthz")
check "GET /healthz" 200 "$status"

echo "== 请求体非 JSON =="
status=$(curl -sS -o /tmp/hk1.json -w '%{http_code}' -X POST "${BASE}/snapshots" \
  -H 'Content-Type: application/json' --data 'not-json')
check "POST bad json → 400" 400 "$status"
cat /tmp/hk1.json; echo

echo "== 缺少必填字段 =="
status=$(curl -sS -o /tmp/hk2.json -w '%{http_code}' -X POST "${BASE}/snapshots" \
  -H 'Content-Type: application/json' --data '{"host":"1.2.3.4"}')
check "POST missing fields → 400" 400 "$status"
cat /tmp/hk2.json; echo

echo "== time 格式非法 =="
status=$(curl -sS -o /tmp/hk3.json -w '%{http_code}' -X POST "${BASE}/snapshots" \
  -H 'Content-Type: application/json' \
  --data '{"host":"1.2.3.4","username":"a","password":"b","time":"2026-13-99 10:00:05"}')
check "POST bad time → 400" 400 "$status"
cat /tmp/hk3.json; echo

echo "== 实时模式连不可达地址（SDK 登录失败，预期 5xx，仅观察） =="
status=$(curl -sS -o /tmp/hk4.json -w '%{http_code}' -X POST "${BASE}/snapshots" \
  -H 'Content-Type: application/json' \
  --data '{"host":"127.0.0.1","port":1,"username":"a","password":"b","timeout_ms":8000}')
echo "POST unreachable host → HTTP $status"
cat /tmp/hk4.json; echo

echo "== 历史模式连不可达地址 =="
status=$(curl -sS -o /tmp/hk5.json -w '%{http_code}' -X POST "${BASE}/snapshots" \
  -H 'Content-Type: application/json' \
  --data '{"host":"127.0.0.1","port":1,"username":"a","password":"b","time":"2026-01-01 00:00:00","timeout_ms":8000}')
echo "POST history unreachable → HTTP $status"
cat /tmp/hk5.json; echo

echo
if [ "$FAIL" = "0" ]; then echo "全部冒烟用例通过"; else echo "存在失败用例"; exit 1; fi
