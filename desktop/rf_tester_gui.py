# -*- coding: utf-8 -*-
"""
RF_tester — اپ دسکتاپ بومی (Tkinter، فقط کتابخانه‌ی استاندارد پایتون)

    python3 desktop/rf_tester_gui.py
    python3 desktop/rf_tester_gui.py --simulate     # بدون سخت‌افزار

روی ویندوز/لینوکس/مک بدون هیچ نصب اضافه‌ای اجرا می‌شود.
اگر نسخه‌ی مرورگری را ترجیح می‌دهید: desktop/server.py
"""

from __future__ import annotations

import argparse
import csv
import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esp_protocol  # noqa: E402

BG = "#0b1220"
PANEL = "#131c2e"
TXT = "#e8eefc"
MUTED = "#8fa3c4"


class App(tk.Tk):
    def __init__(self, host: str, port: int):
        super().__init__()
        self.title("RF Tester — کلاینت ESP32")
        self.geometry("880x560")
        self.minsize(720, 460)
        self.configure(bg=BG)

        self.records = []
        self.q: "queue.Queue" = queue.Queue()

        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("Treeview", background=PANEL, fieldbackground=PANEL,
                        foreground=TXT, rowheight=26, borderwidth=0)
        style.configure("Treeview.Heading", background="#1b2740", foreground=TXT)
        style.map("Treeview", background=[("selected", "#2b6cb0")])

        # --- نوار بالا ---
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=14, pady=12)

        tk.Label(bar, text="IP:", bg=BG, fg=MUTED).pack(side="left")
        self.host_var = tk.StringVar(value=host)
        tk.Entry(bar, textvariable=self.host_var, width=16, bg="#0a1120", fg=TXT,
                 insertbackground=TXT, relief="flat").pack(side="left", padx=(4, 12), ipady=4)

        tk.Label(bar, text="Port:", bg=BG, fg=MUTED).pack(side="left")
        self.port_var = tk.StringVar(value=str(port))
        tk.Entry(bar, textvariable=self.port_var, width=7, bg="#0a1120", fg=TXT,
                 insertbackground=TXT, relief="flat").pack(side="left", padx=(4, 12), ipady=4)

        self.btn1 = tk.Button(bar, text="آخرین رکورد (sync)", command=lambda: self.run("sync"),
                              bg="#3b82f6", fg="#06101f", relief="flat", padx=12, pady=5)
        self.btn1.pack(side="left", padx=4)
        self.btn10 = tk.Button(bar, text="۱۰ رکورد آخر (sync10)", command=lambda: self.run("sync10"),
                               bg="#22d3ee", fg="#06101f", relief="flat", padx=12, pady=5)
        self.btn10.pack(side="left", padx=4)
        tk.Button(bar, text="خروجی CSV", command=self.export_csv, bg=PANEL, fg=TXT,
                  relief="flat", padx=12, pady=5).pack(side="left", padx=4)
        tk.Button(bar, text="پاک کردن", command=self.clear, bg=PANEL, fg=TXT,
                  relief="flat", padx=12, pady=5).pack(side="left", padx=4)

        # --- جدول ---
        cols = ("id", "time", "temp", "hum", "n1", "n2")
        titles = ("ID", "زمان دستگاه", "دما (C)", "رطوبت (%)", "NBCM1", "NBCM2")
        widths = (70, 190, 100, 110, 100, 100)
        self.tree = ttk.Treeview(self, columns=cols, show="headings")
        for c, t, w in zip(cols, titles, widths):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="center")
        self.tree.tag_configure("bad", foreground="#fca5a5")
        self.tree.tag_configure("good", foreground="#86efac")
        self.tree.pack(fill="both", expand=True, padx=14)

        # --- پایین ---
        self.status = tk.Label(self, text="آماده — ابتدا به هات‌اسپات ESP32 وصل شوید.",
                               bg=BG, fg=MUTED, anchor="w")
        self.status.pack(fill="x", padx=16, pady=(8, 2))

        tk.Label(self, text="پاسخ خام:", bg=BG, fg=MUTED, anchor="w").pack(fill="x", padx=16)
        self.raw = tk.Text(self, height=6, bg="#080e1a", fg="#9fd3ff", relief="flat",
                           insertbackground=TXT)
        self.raw.pack(fill="x", padx=14, pady=(0, 12))

        self.after(120, self._drain)

    # ---------------------------------------------------------------- منطق
    def set_busy(self, busy: bool):
        state = "disabled" if busy else "normal"
        self.btn1.config(state=state)
        self.btn10.config(state=state)

    def run(self, cmd: str):
        host = self.host_var.get().strip() or "192.168.1.1"
        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            messagebox.showerror("خطا", "پورت نامعتبر است")
            return

        self.set_busy(True)
        self.status.config(text=f"در حال ارسال «{cmd}» به {host}:{port} …", fg="#f59e0b")

        def worker():
            try:
                raw = esp_protocol.query(host, port, cmd)
                self.q.put(("ok", cmd, raw))
            except Exception as exc:
                self.q.put(("err", cmd, f"{type(exc).__name__}: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    def _drain(self):
        try:
            while True:
                kind, cmd, payload = self.q.get_nowait()
                self.set_busy(False)
                if kind == "err":
                    self.status.config(text="خطا: " + payload, fg="#ef4444")
                    self._set_raw(payload)
                    continue

                self._set_raw(payload or "—")
                if not payload:
                    self.status.config(text="پاسخی نیامد (تایم‌اوت)", fg="#ef4444")
                elif esp_protocol.is_no_data(payload):
                    self.status.config(text="دستگاه رکوردی روی SD ندارد", fg="#ef4444")
                else:
                    recs = esp_protocol.parse_records(payload)
                    if not recs:
                        self.status.config(text="پاسخ قابل پارس نبود", fg="#ef4444")
                    else:
                        self.records = sorted(recs, key=lambda r: r.id, reverse=True)
                        self._fill(self.records)
                        self.status.config(text=f"{len(recs)} رکورد دریافت شد.", fg="#22c55e")
        except queue.Empty:
            pass
        self.after(120, self._drain)

    def _set_raw(self, text: str):
        self.raw.delete("1.0", "end")
        self.raw.insert("1.0", text)

    def _fill(self, recs):
        self.tree.delete(*self.tree.get_children())
        for r in recs:
            tag = "good" if (r.nbcm1 and r.nbcm2) else "bad"
            self.tree.insert("", "end", tags=(tag,), values=(
                r.id, r.timestamp, f"{r.temp:.2f}", f"{r.humidity:.2f}",
                "OK" if r.nbcm1 else "NOK", "OK" if r.nbcm2 else "NOK"))

    def clear(self):
        self.records = []
        self.tree.delete(*self.tree.get_children())
        self._set_raw("")
        self.status.config(text="پاک شد.", fg=MUTED)

    def export_csv(self):
        if not self.records:
            messagebox.showinfo("CSV", "چیزی برای خروجی گرفتن نیست.")
            return
        path = filedialog.asksaveasfilename(defaultextension=".csv",
                                            initialfile="rf_tester_export.csv",
                                            filetypes=[("CSV", "*.csv")])
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["ID", "Timestamp", "Temp", "Humidity", "NBCM1", "NBCM2"])
            for r in self.records:
                w.writerow([r.id, r.timestamp, r.temp, r.humidity,
                            "OK" if r.nbcm1 else "NOK", "OK" if r.nbcm2 else "NOK"])
        self.status.config(text="ذخیره شد: " + path, fg="#22c55e")


def main():
    ap = argparse.ArgumentParser(description="RF_tester desktop GUI")
    ap.add_argument("--esp-host", default="192.168.1.1")
    ap.add_argument("--esp-port", type=int, default=80)
    ap.add_argument("--simulate", action="store_true", help="اجرای شبیه‌ساز داخلی")
    args = ap.parse_args()

    host, port = args.esp_host, args.esp_port
    if args.simulate:
        import simulator
        simulator.start_background("127.0.0.1", 9080)
        host, port = "127.0.0.1", 9080

    App(host, port).mainloop()


if __name__ == "__main__":
    main()
