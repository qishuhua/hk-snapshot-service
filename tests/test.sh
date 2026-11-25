# 实时截图
curl -sS -X POST http://localhost:8180/snapshots \
  -H 'Content-Type: application/json' \
  -d '{"host":"192.168.60.13","port":8030,"username":"admin","password":"jtxa@2026","channel":1}' \
  -o now.jpg

curl -sS -X POST http://localhost:8180/snapshots \
  -H 'Content-Type: application/json' \
  -d '{"host":"192.168.60.13","port":8030,"username":"admin","password":"jtxa@2026","channel":1,
       "time":"2026-11-19 10:00:05"}' \
  -o frame.jpg -D headers.txt