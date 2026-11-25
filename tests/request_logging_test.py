#!/usr/bin/env python3
"""启动隔离服务，验证请求输入、响应、失败分支和并发日志关联。"""

import argparse
import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time


@contextlib.contextmanager
def service(binary, directory):
    directory.mkdir()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    environment = dict(os.environ, HKMEDIA_LISTEN_ADDR="127.0.0.1", HKMEDIA_PORT=str(port),
                       HKMEDIA_LOG_LEVEL="info",
                       HKMEDIA_MAX_CONCURRENT="1", HKMEDIA_CONNECT_TIMEOUT_MS="1000",
                       LOGDIR=str(directory), APPNAME="hk-media-log-test", HKMEDIA_SDK_LOG="0")
    with (directory / "console.log").open("w") as console:
        process = subprocess.Popen([str(binary)], cwd=directory, env=environment,
                                   stdout=console, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("测试服务启动失败")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError("等待测试服务启动超时")
            yield port, directory / "hk-media-log-test.log"
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def request(port, method, path, payload, expected, headers=None):
    body = json.dumps(payload).encode() if isinstance(payload, dict) else payload
    request_headers = {"Content-Type": "application/json", "X-Request-Id": "client-supplied"}
    request_headers.update(headers or {})
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        connection.request(method, path, body=body, headers=request_headers)
        response = connection.getresponse()
        output = response.read()
        request_id = response.getheader("X-Request-Id")
        assert response.status == expected, (path, response.status, output)
        assert request_id and request_id != "client-supplied", request_id
        return {"id": request_id, "status": response.status, "body": body or b"", "output": output}
    finally:
        connection.close()


def read_events(path):
    pattern = re.compile(r"\[request_id=([^\]]+)\] \[http\] ([a-z_.]+) (\{.*\})$")
    events = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.search(line)
        if match:
            request_id, event, details = match.groups()
            events.setdefault(request_id, {}).setdefault(event, []).append(json.loads(details))
    return events


def verify_records(path, records):
    deadline = time.monotonic() + 3
    while True:
        events = read_events(path)
        if all("request.complete" in events.get(record["id"], {}) for record in records):
            break
        assert time.monotonic() < deadline, "请求结束日志缺失"
        time.sleep(0.02)
    assert len({record["id"] for record in records}) == len(records), "请求 ID 重复"
    for record in records:
        trace = events[record["id"]]
        assert len(trace["request.start"]) == len(trace["request.body"]) == 1
        assert trace["request.complete"][0]["status"] == record["status"]
        assert trace["response.ready"][0]["body_bytes"] == len(record["output"])
        assert trace["response.ready"][0]["body"] == record["output"].decode()
        # 超过框架接收上限时只校验有输入记录，避免要求记录未被框架接受的内容。
        if record["status"] != 413:
            assert trace["request.body"][0]["body"] == record["body"].decode(errors="replace")
    return events


def verify_busy(port, records):
    accepted = threading.Event()
    release = threading.Event()
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    device_port = listener.getsockname()[1]

    def hold_device_connection():
        with listener:
            connection, _ = listener.accept()
            with connection:
                accepted.set()
                release.wait(timeout=10)

    thread = threading.Thread(target=hold_device_connection, daemon=True)
    thread.start()
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        future = executor.submit(request, port, "POST", "/snapshots",
                                 {"host": "127.0.0.1", "port": device_port,
                                  "username": "busy-probe", "password": "busy-password"}, 502)
        try:
            assert accepted.wait(timeout=10), "未能建立测试设备连接"
            records.append(request(port, "POST", "/snapshots",
                                   {"host": "127.0.0.1", "port": device_port,
                                    "username": "busy-probe", "password": "rejected-password"}, 429))
            records.append(request(port, "POST", "/snapshots",
                                   {"host": "127.0.0.1", "port": 1,
                                    "username": "other-device", "password": "test"}, 502))
        finally:
            release.set()
        records.append(future.result(timeout=15))
    thread.join(timeout=1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="hk-media-request-logs-") as temporary:
        root = Path(temporary)
        with service(binary, root / "open") as (port, log_file):
            records = [request(port, "GET", "/healthz", None, 200),
                       request(port, "GET", "/unknown?probe=1", None, 404),
                       request(port, "POST", "/api/v1/capture", b"bad-json", 400),
                       request(port, "POST", "/api/v1/download", b"bad-json", 404),
                       request(port, "POST", "/api/snapshots", b"bad-json", 400),
                       request(port, "POST", "/api/recording-clips", b"bad-json", 404),
                       request(port, "POST", "/recording-clips", b"bad-json", 404),
                       request(port, "POST", "/recordings", b"bad-json", 404),
                       request(port, "POST", "/recordings/download", b"bad-json", 404),
                       request(port, "POST", "/snapshots", b"not-json\nsecond-line", 400),
                       request(port, "POST", "/snapshots", b'{"invalid":"\xff"}', 400),
                       request(port, "POST", "/snapshots", b"x" * 65537, 413)]
            records.append(request(port, "POST", "/snapshots",
                                   {"host": "127.0.0.1", "username": "test",
                                    "password": "test", "time": "2026-11-19 10:00:05.123"}, 400))
            password = 'plain-password\n"quoted"'
            login = request(port, "POST", "/snapshots",
                            {"host": "127.0.0.1", "port": 1, "username": "diag-user",
                             "password": password}, 502)
            records.append(login)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
                records.extend(executor.map(lambda _: request(port, "GET", "/healthz", None, 200), range(8)))
            verify_busy(port, records)
            events = verify_records(log_file, records)
            params = events[login["id"]]["capture.params"][0]
            assert params["password"] == password, "密码被脱敏或丢失"
            assert params["channel"] == 1 and params["picture_size"] == 255
            print("PASS: 输入输出、明文密码、默认参数、400/404/413/429/502 与并发请求 ID")



if __name__ == "__main__":
    main()
