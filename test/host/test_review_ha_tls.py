import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "components/ha_client/ha_client.c").read_text()
cmake = (ROOT / "components/ha_client/CMakeLists.txt").read_text()
config = re.search(r"esp_http_client_config_t config\s*=\s*\{(.*?)\};", source, re.S)
assert config is not None
assert '#include "esp_crt_bundle.h"' in source
assert re.search(r"\.crt_bundle_attach\s*=\s*esp_crt_bundle_attach\b", config.group(1))
assert "skip_cert_common_name_check" not in config.group(1)
assert "mbedtls" in cmake.split("PRIV_REQUIRES", 1)[1]
print("review HA TLS config: passed")
