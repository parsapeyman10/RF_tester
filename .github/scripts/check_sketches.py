#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
بررسی ایستای اسکچ‌های Arduino بدون نیاز به کامپایلر.

دقیقاً همان دسته خطاهایی را می‌گیرد که در عمل به آن‌ها خوردیم:
  1. نام سراسری که با توابع استاندارد C تداخل دارد (مثل link در unistd.h)
  2. تابعی که پروتوتایپ دارد ولی تعریفش وجود ندارد (خطای لینکر)
  3. تابعی که قبل از تعریفش صدا زده شده و پروتوتایپ هم ندارد
  4. ناهماهنگی آکولاد و پرانتز
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKETCHES = [
    os.path.join(ROOT, "esp32_controller_FIXED", "esp32_controller_FIXED.ino"),
    os.path.join(ROOT, "esp8266_receiver_FIXED", "esp8266_receiver_FIXED.ino"),
]

RESERVED_GLOBALS = {
    "link", "index", "time", "read", "write", "close", "open", "remove",
    "div", "exit", "log", "log2", "log10", "exp", "pow", "clock", "pipe",
    "fork", "wait", "signal", "connect", "socket", "select", "send", "recv",
    "bind", "listen", "accept", "y0", "y1", "j0", "j1", "gamma", "stat",
}

KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "case", "sizeof",
    "defined", "new", "delete", "catch", "typedef", "struct", "class", "enum",
    "namespace", "template", "operator", "public", "private", "protected",
}

failures = []


def fail(msg):
    failures.append(msg)
    print(f"  FAIL- {msg}")


def ok(msg):
    print(f"  ok  - {msg}")


def strip_literals(src):
    """رشته‌ها، کاراکترها، raw stringها و کامنت‌ها را خالی می‌کند (خط‌ها حفظ می‌شوند)"""
    src = re.sub(r'R"(\w*)\(.*?\)\1"', '""', src, flags=re.S)
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    out = []
    for line in src.split("\n"):
        # ترتیب مهم است: اول رشته‌ها، بعد کامنت‌ها. اگر برعکس باشد، آدرسی
        # مثل "http://192.168.1.1/" داخل رشته، نصف خط را به‌عنوان کامنت می‌برد.
        line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
        line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
        line = re.sub(r"//.*$", "", line)
        out.append(line)
    return "\n".join(out)


def remove_class_bodies(code):
    """بدنه‌ی class/struct حذف می‌شود؛ قواعد اسکوپ داخلشان فرق دارد"""
    result = []
    i = 0
    while True:
        m = re.search(r"\b(?:class|struct)\s+\w+[^;{]*\{", code[i:])
        if not m:
            result.append(code[i:])
            break
        start = i + m.start()
        result.append(code[i:start])
        depth = 0
        j = i + m.end() - 1
        while j < len(code):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        i = j + 1
    return "".join(result)


def looks_like_prototype(params):
    """پارامترهای یک پروتوتایپ واقعی، «نوع + نام» هستند؛ نه مقدارِ سازنده"""
    params = params.strip()
    if params in ("", "void"):
        return True
    for prm in params.split(","):
        prm = prm.strip()
        if not prm:
            return False
        if re.match(r'^[\d"\'+-]', prm):   # IPAddress ip(192,168,4,1);
            return False
        if " " not in prm and "*" not in prm and "&" not in prm:
            return False                   # WiFiServer server(SERVER_PORT);
    return True


def check_sketch(path):
    name = os.path.basename(path)
    print(f"\n=== {name} ===")
    code = strip_literals(open(path, encoding="utf-8").read())

    # ---- 1) توازن آکولاد و پرانتز ----
    for op, cl, label in (("{", "}", "آکولاد"), ("(", ")", "پرانتز")):
        if code.count(op) != code.count(cl):
            fail(f"{name}: {label} نامتوازن است ({code.count(op)} / {code.count(cl)})")
        else:
            ok(f"{label} متوازن است")

    # ---- 2) نام سراسری با تداخل ----
    top_level = remove_class_bodies(code)
    # فقط تعریف‌های ستون صفر = واقعاً سراسری (متغیر محلیِ هم‌نام مشکلی ندارد)
    globals_found = re.findall(
        r"^(?:static\s+|const\s+|volatile\s+)*[A-Za-z_]\w*(?:\s*[*&])?\s+(\w+)\s*(?:=|;|\[)",
        top_level, re.M)
    clashes = sorted(set(g for g in globals_found if g in RESERVED_GLOBALS))
    if clashes:
        for c in clashes:
            fail(f"{name}: نام سراسری '{c}' با تابع استاندارد C تداخل دارد "
                 f"(خطای redeclared as different kind of entity)")
    else:
        ok("هیچ نام سراسری با توابع استاندارد C تداخل ندارد")

    # ---- 3) تعریف‌ها و پروتوتایپ‌ها ----
    flat = re.sub(r"[ \t]*\n[ \t]*", " ", top_level)
    flat = re.sub(r"\s{2,}", " ", flat)

    # از lookbehind استفاده می‌کنیم تا ';' جداکننده مصرف نشود؛ وگرنه
    # پروتوتایپ‌های پشت‌سرهم یکی‌درمیان از قلم می‌افتند.
    sig = (r"(?:(?<=[;}])|(?<=^))\s*(?:static\s+|inline\s+)*"
           r"(?:const\s+)?[A-Za-z_][\w:]*(?:\s*[*&]\s*|\s+)([A-Za-z_]\w*)\s*\(([^()]*)\)\s*")

    defs, protos = {}, {}
    for m in re.finditer(sig + r"\{", flat):
        if m.group(1) not in KEYWORDS:
            defs.setdefault(m.group(1), m.start())
    for m in re.finditer(sig + r";", flat):
        fname, params = m.group(1), m.group(2)
        if fname in KEYWORDS or not looks_like_prototype(params):
            continue
        protos.setdefault(fname, m.start())

    missing = sorted(n for n in protos if n not in defs)
    if missing:
        for n in missing:
            fail(f"{name}: تابع '{n}' پروتوتایپ دارد ولی تعریف نشده (خطای لینکر)")
    else:
        ok(f"هر {len(protos)} پروتوتایپ، تعریف متناظر دارد")

    # ---- 4) فراخوانی قبل از تعریف بدون پروتوتایپ ----
    early = []
    for fname, dpos in defs.items():
        if fname in protos or fname in ("setup", "loop"):
            continue
        for m in re.finditer(r"\b" + re.escape(fname) + r"\s*\(", flat):
            if m.start() < dpos:
                early.append(fname)
                break
    if early:
        for n in sorted(set(early)):
            fail(f"{name}: '{n}' قبل از تعریفش صدا زده شده و پروتوتایپ ندارد")
    else:
        ok("هر تابعی که زودتر صدا زده شده، پروتوتایپ دارد")


for sk in SKETCHES:
    if not os.path.isfile(sk):
        fail(f"اسکچ پیدا نشد: {sk}")
    else:
        check_sketch(sk)

print()
if failures:
    for f in failures:
        print(f"::error::{f}")
    print(f"{len(failures)} مشکل در اسکچ‌ها پیدا شد.")
    sys.exit(1)
print("اسکچ‌ها از نظر ساختاری سالم‌اند.")
