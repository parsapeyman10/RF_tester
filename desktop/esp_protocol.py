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
NO_DATA = "NO_DATA"

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
    out: List[Reading] = []
    for chunk in _OBJ_RE.findall(raw or ""):
        rid = _field(chunk, "ID")
        if rid is None:
            continue
        rid_i = _to_int(rid, -1)
        if rid_i < 0:
            continue
        out.append(
            Reading(
                id=rid_i,
                temp=_to_float(_field(chunk, "T")),
                humidity=_to_float(_field(chunk, "H")),
                nbcm1=_to_int(_field(chunk, "N1")) != 0,
                nbcm2=_to_int(_field(chunk, "N2")) != 0,
                timestamp=_field(chunk, "Time") or "",
            )
        )
    return out


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
            if NO_DATA in text:
                break
            if text.rstrip().endswith("]"):
                break
            if command == CMD_SYNC_LAST and "}" in text:
                break
        return buf.decode("utf-8", "ignore").strip()
