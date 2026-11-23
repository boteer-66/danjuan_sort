import argparse
import getpass
import os
import sys

try:
    import tos
except ImportError:
    print("缺少 Python SDK：请先执行 `pip install tos`", file=sys.stderr)
    sys.exit(1)


def read_value(cli_value: str, env_name: str, prompt: str, secret: bool = False) -> str:
    if cli_value:
        return cli_value

    env_value = os.getenv(env_name)
    if env_value:
        return env_value

    if secret:
        try:
            return getpass.getpass(f"{prompt}: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\n当前终端不支持隐藏输入，改为普通可见输入。")
            return input(f"{prompt}: ").strip()

    return input(f"{prompt}: ").strip()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="生成火山 TOS 的 PUT 预签名 URL，用于 ESP32 直传对象。"
    )
    parser.add_argument("--bucket", help="桶名，例如 the-test-tos")
    parser.add_argument("--key", help="对象名，例如 esp32/test-upload.txt")
    parser.add_argument("--expires", type=int, default=7200, help="有效期，单位秒，默认 7200")
    parser.add_argument("--endpoint", help="外网 endpoint，例如 tos-cn-beijing.volces.com")
    parser.add_argument("--region", help="地域，例如 cn-beijing")
    parser.add_argument("--ak", help="AccessKeyID")
    parser.add_argument("--sk", help="AccessKeySecret")
    parser.add_argument("--token", help="STS token，没有就留空")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    bucket = read_value(args.bucket, "TOS_BUCKET", "Bucket")
    key = read_value(args.key, "TOS_KEY", "Object Key")
    endpoint = read_value(args.endpoint, "TOS_ENDPOINT", "Endpoint")
    region = read_value(args.region, "TOS_REGION", "Region")
    ak = read_value(args.ak, "TOS_ACCESS_KEY", "AccessKeyID")
    sk = read_value(args.sk, "TOS_SECRET_KEY", "AccessKeySecret", secret=True)
    token = read_value(args.token, "TOS_SECURITY_TOKEN", "SecurityToken(可留空)")

    if not bucket or not key or not endpoint or not region or not ak or not sk:
        print("必要参数不完整，无法生成预签名 URL。", file=sys.stderr)
        return 1

    if args.expires <= 0:
        print("expires 必须大于 0。", file=sys.stderr)
        return 1

    auth = tos.Auth(ak, sk, region)
    client = tos.TosClient(auth, endpoint)

    kwargs = {
        "Method": "PUT",
        "Bucket": bucket,
        "Key": key,
        "ExpiresIn": args.expires,
    }
    if token:
        kwargs["SecurityToken"] = token

    signed_url = client.generate_presigned_url(**kwargs)

    print("\n生成成功。把下面这个完整 URL 原样复制到 main.cpp 里：\n")
    print(signed_url)
    print("\n建议：复制后立刻烧录测试，避免链接过期。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
