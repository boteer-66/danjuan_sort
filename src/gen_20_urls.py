#!/usr/bin/env python3
"""批量生成 20 个 TOS PUT 预签名 URL，直接输出 C 数组"""
import getpass, os, sys

try: import tos
except ImportError: print("请先执行 pip install tos"); sys.exit(1)

COUNT    = 20
PREFIX   = "esp32/patrol"
BUCKET   = "the-test-tos"
ENDPOINT = "tos-cn-beijing.volces.com"
REGION   = "cn-beijing"
EXPIRES  = 86400  # 24小时，拍视频不用赶2h

def prompt(name, secret=False):
    env = os.getenv(f"TOS_{name.upper()}")
    if env: return env
    return getpass.getpass(f"{name}: ") if secret else input(f"{name}: ")

print("=== 生成 20 个 TOS 预签名 URL ===\n")
ak = prompt("AccessKeyID")
sk = prompt("AccessKeySecret", secret=True)
print()

auth   = tos.Auth(ak, sk, REGION)
client = tos.TosClient(auth, ENDPOINT)

urls = []
for i in range(1, COUNT + 1):
    key = f"{PREFIX}-{i:02d}.jpg"
    url = client.generate_presigned_url(Method="PUT", Bucket=BUCKET, Key=key, ExpiresIn=EXPIRES)
    urls.append(url)
    print(f"  [{i:2d}/20] {key}")

print("\n// ===== 粘贴到 main.cpp 的 TOS_URLS[] 数组中 =====\n")
print("static const char* TOS_URLS[] = {")
for i, url in enumerate(urls):
    comma = "," if i < len(urls) - 1 else ""
    print(f'  "{url}"{comma}')
print("};")
print(f"\nstatic constexpr int kTosUrlCount = {len(urls)};")
print("\n// 复制上面代码替换 main.cpp 里原来的 TOS_URLS[] 即可")
