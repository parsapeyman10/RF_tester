# -*- coding: utf-8 -*-
"""
پروتکل مشترک کلاینت‌های دسکتاپ RF_tester.

هم‌ارز دقیق EspProtocol.kt / EspClient.kt در اپ اندروید:

    ESP32 (MODE_HOTSPOT_VIEW) --TCP--> کلاینت
        فرستادن "sync\\n"   -> آخرین رکورد
        فرستادن "sync10\\n" -> حداکثر ۱۰ رکورد داخل آرایه [ ... ]
        پاسخ هر رکورد:
            {"ID":42,"T":23.45,"H":51.20,"N1":1,"N2":0,"Time":"2026-01-05 13:04:09"}
        نبود دیتا: NO_DATA

نکته: فریمور بعد از پاسخ سوکت را نمی‌بندد، پس تا EOF نمی‌خوانیم؛
با تایم‌اوت و تشخیص پایان پاسخ ('}' یا ']') کار را تمام می‌کنیم.
"""

from __future__ import annotations

import re
import socket
from dataclasses import dataclass, asdict
from typing import List

CMD_SYNC_LAST = "sync"
CMD_SYNC_10 = "sync10"
CMD_SYNC_ALL = "syncall"
CMD_INFO = "info"
NO_DATA = "NO_DATA"
END_MARK = "END"

# فرمت واحد پروژه: همان خطی که app.py هم می‌خواند
LINE_RE = re.compile(
    r"NUM=(?P<num>-?\d+),"
    r"NBCM1=(?P<n1>[A-Za-z0-9]+),NBCM2=(?P<n2>[A-Za-z0-9]+),"
    r"NBCM3=(?P<n3>[A-Za-z0-9]+),NBCM4=(?P<n4>[A-Za-z0-9]+),"
    r"Temp=(?P<t>-?\d+(?:\.\d+)?),Humidity=(?P<h>-?\d+(?:\.\d+)?),"
    r"Date=(?P<y>\d{4})-(?P<mo>\d{1,2})-(?P<d>\d{1,2}),"
    r"Time=(?P<hh>\d{1,2}):(?P<mi>\d{1,2}):(?P<ss>\d{1,2})"
)
TRUE_TOKENS = ("OK", "1", "TRUE", "YES")

_OBJ_RE = re.compile(r"\{[^{}]*\}")


def _field(chunk: str, key: str):
    m = re.search(r'"%s"\s*:\s*"?([^,"}]*)"?' % re.escape(key), chunk)
    return m.group(1).strip() if m else None


@dataclass
class Reading:
    id: int
    temp: float
    humidity: float
    nbcm1: bool
    nbcm2: bool
    timestamp: str
    nbcm3: bool = False
    nbcm4: bool = False
    raw_line: str = ""      # خط خام NUM=... برای ارسال بی‌کم‌وکاست به app.py

    def to_dict(self):
        return asdict(self)

    def pretty(self) -> str:
        return (
            f"#{self.id}   {self.timestamp}\n"
            f"    T = {self.temp:.2f} C    |    H = {self.humidity:.2f} %\n"
            f"    NBCM1 = {'OK' if self.nbcm1 else 'NOK'}    |    "
            f"NBCM2 = {'OK' if self.nbcm2 else 'NOK'}"
        )


def is_no_data(raw: str) -> bool:
    return NO_DATA in raw


def _to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return float("nan")


def _to_int(v, default=0):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def parse_records(raw: str) -> List[Reading]:
    """
    پاسخ دستگاه را به رکورد تبدیل می‌کند.
    اولویت با فرمت واحد «NUM=...» است (همان چیزی که app.py می‌فهمد)؛
    برای سازگاری با فریمورهای قدیمی، فرمت JSON هم پشتیبانی می‌شود.
    """
    out: List[Reading] = []

    for line in (raw or "").splitlines():
        line = line.strip()
        if not line or line in (END_MARK, NO_DATA):
            continue
        m = LINE_RE.search(line)
        if not m:
            continue
        g = m.groupdict()
        out.append(Reading(
            id=_to_int(g["num"], -1),
            temp=_to_float(g["t"]),
            humidity=_to_float(g["h"]),
            nbcm1=g["n1"].upper() in TRUE_TOKENS,
            nbcm2=g["n2"].upper() in TRUE_TOKENS,
            nbcm3=g["n3"].upper() in TRUE_TOKENS,
            nbcm4=g["n4"].upper() in TRUE_TOKENS,
            timestamp="%04d-%02d-%02d %02d:%02d:%02d" % (
                int(g["y"]), int(g["mo"]), int(g["d"]),
                int(g["hh"]), int(g["mi"]), int(g["ss"])),
            raw_line=m.group(0),
        ))

    if out:
        return out

    # --- سازگاری عقب‌رو: فرمت JSON نسخه‌های قبلی ---
    for chunk in _OBJ_RE.findall(raw or ""):
        rid = _field(chunk, "ID")
        if rid is None:
            continue
        rid_i = _to_int(rid, -1)
        if rid_i < 0:
            continue
        out.append(Reading(
            id=rid_i,
            temp=_to_float(_field(chunk, "T")),
            humidity=_to_float(_field(chunk, "H")),
            nbcm1=_to_int(_field(chunk, "N1")) != 0,
            nbcm2=_to_int(_field(chunk, "N2")) != 0,
            timestamp=_field(chunk, "Time") or "",
        ))
    return out


def to_server_lines(records: List[Reading]) -> List[str]:
    """خط‌های آماده برای POST به /api/ingest در app.py"""
    lines = []
    for r in records:
        if r.raw_line:
            lines.append(r.raw_line)
        else:
            lines.append(
                "NUM=%d,NBCM1=%s,NBCM2=%s,NBCM3=%s,NBCM4=%s,"
                "Temp=%.2f,Humidity=%.2f,Date=%s,Time=%s" % (
                    r.id,
                    "OK" if r.nbcm1 else "NOK", "OK" if r.nbcm2 else "NOK",
                    "OK" if r.nbcm3 else "NOK", "OK" if r.nbcm4 else "NOK",
                    r.temp, r.humidity,
                    (r.timestamp.split(" ")[0] if r.timestamp else "1970-01-01"),
                    (r.timestamp.split(" ")[1] if " " in r.timestamp else "00:00:00"),
                ))
    return lines


def send_to_app_server(server_url: str, records: List[Reading], device: str = "desktop"):
    """رکوردها را به /api/ingest سرور Flask می‌فرستد (فقط کتابخانه‌ی استاندارد)."""
    import json as _json
    import urllib.request

    url = server_url.rstrip("/") + "/api/ingest"
    body = _json.dumps({"lines": to_server_lines(records)}).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-Device", device)
    with urllib.request.urlopen(req, timeout=10) as resp:
        return _json.loads(resp.read().decode("utf-8", "ignore"))


def query(host: str, port: int, command: str,
          connect_timeout: float = 4.0,
          read_timeout: float = 1.5,
          overall_timeout: float = 8.0) -> str:
    """دستور را می‌فرستد و پاسخ خام را برمی‌گرداند."""
    import time

    deadline = time.time() + overall_timeout
    with socket.create_connection((host, int(port)), timeout=connect_timeout) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(read_timeout)
        s.sendall((command + "\n").encode("ascii"))

        buf = b""
        while time.time() < deadline:
            try:
                chunk = s.recv(1024)
            except socket.timeout:
                if buf:
                    break
                continue
            if not chunk:
                break
            buf += chunk
            text = buf.decode("utf-8", "ignore")
            if END_MARK in text.splitlines()[-1:] or text.rstrip().endswith("\nEND"):
                break
            if text.strip() == NO_DATA:
                break
            if text.rstrip().endswith("]"):      # فریمور قدیمی (JSON)
                break
            if command == CMD_SYNC_LAST and "}" in text:
                break
        return buf.decode("utf-8", "ignore").strip()
