# -*- coding: utf-8 -*-
"""نقطه‌ی ورود zipapp؛ منطق اصلی در launcher.py است."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from launcher import main  # noqa: E402

if __name__ == "__main__":
    main()
