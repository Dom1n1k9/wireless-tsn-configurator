#!/usr/bin/env python3
"""WTSN Configurator - web GUI launcher (thin shim).

The implementation lives in the wtsn_webgui/ package. Kept at the repo root
so `python3 webgui.py [--host H] [--port P]` keeps working.
"""
from wtsn_webgui.__main__ import main

if __name__ == "__main__":
    raise SystemExit(main())
