from pathlib import Path
import json
from urllib.parse import urlsplit
root=Path(__file__).resolve().parent
config=json.loads((root/"config.local.json").read_text())
url=config["server_url"];token=config["device_token"]
parsed=urlsplit(url)
if parsed.scheme not in ("ws","wss") or not parsed.hostname or parsed.path!="/xiaozhi/v1/" or parsed.username or parsed.password or parsed.query or parsed.fragment:
    raise SystemExit("Expected ws[s]://host:port/xiaozhi/v1/")
if not isinstance(token,str) or not 16<=len(token)<=64 or any(not c.isalnum() and c not in "-_" for c in token):
    raise SystemExit("device_token must be 16-64 URL-safe characters")
out=root/"firmware/generated";out.mkdir(parents=True,exist_ok=True)
(out/"probe_config.h").write_text("#pragma once\n#define WC_SERVER_URL "+json.dumps(url)+"\n#define WC_DEVICE_TOKEN "+json.dumps(token)+"\n",encoding="ascii")
print("Prepared local firmware settings (token omitted)")
