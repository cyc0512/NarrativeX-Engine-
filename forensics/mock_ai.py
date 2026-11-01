#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
T03 取证用的本地 mock AI 服务（仅用于复现与回归，不参与生产）。

用途：
  1. R-02：返回含中文 / emoji / 中文引号「」的 JSON，验证清洗逻辑有没有把 UTF-8 吃光。
  2. R-03：用 --delay 让响应故意慢于 qt_main.cpp 的 5 秒超时，
           把"AI 超 5 秒"从偶发变成必然，从而确定性地触发 D-4 悬空 watcher。

用法：
  python3 mock_ai.py --port 18080 --delay 0
  python3 mock_ai.py --port 18080 --delay 8      # 8 秒，必超 5s 超时
"""
import argparse
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

# 故意覆盖三类会被"只保留 ASCII"清洗逻辑杀掉的字符：
#   中文汉字、emoji（4 字节 UTF-8）、中文引号「」、全角标点。
REPLY = (
    "哥哥，你回来了【中文测试】。"
    "我看到你口袋里露出一角，那是「发卡」吗？"
    "我有点在意……🌟🌙 "
    "不过还是先吃饭吧，菜要凉了。"
)


class Handler(BaseHTTPRequestHandler):
    delay = 0.0

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)
        if Handler.delay > 0:
            time.sleep(Handler.delay)
        body = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "affinity_change": 2,
                                "next_node": "",
                                "reply": REPLY,
                            },
                            ensure_ascii=False,
                        )
                    }
                }
            ]
        }
        raw = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] %s\n" % (fmt % args))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--delay", type=float, default=0.0)
    a = ap.parse_args()
    Handler.delay = a.delay
    print("mock AI on 127.0.0.1:%d  delay=%.1fs" % (a.port, a.delay), flush=True)
    HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
