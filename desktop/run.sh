#!/usr/bin/env bash
# RF Tester - لینوکس/مک
cd "$(dirname "$0")"
case "${1:-gui}" in
  web) shift; exec python3 server.py "$@" ;;
  *)   exec python3 rf_tester_gui.py "$@" ;;
esac
