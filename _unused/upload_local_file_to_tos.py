import argparse
import getpass
import mimetypes
import os
import sys
import urllib.request

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
        description="把本地文件通过 PUT 预签名 URL 上传到火山 TOS。"
    )
    parser.add_argument("--file", default="src/text.jpg", help="本地文件路径，默认 src/text.jpg")
    parser.add_argument("--bucket", help="桶名，例如 the-test-tos")
    parser.add_argument("--key", help="对象名，例如 esp32/text.jpg")
    parser.add_argument("--expires", type=int, default=7200, help="预签名有效期，单位秒，默认 7200")
    parser.add_argument("--endpoint", help="外网 endpoint，例如 tos-cn-beijing.volces.com")
    parser.add_argument("--region", help="地域，例如 cn-beijing")
    parser.add_argument("--ak", help="AccessKeyID")
    parser.add_argument("--sk", help="AccessKeySecret")
    parser.add_argument("--token", help="STS token，没有就留空")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    file_path = args.file
    if not os.path.exists(file_path):
        print(f"本地文件不存在：{file_path}", file=sys.stderr)
        return 1

    bucket = read_value(args.bucket, "TOS_BUCKET", "Bucket")
    default_key = os.path.basename(file_path)
    key = read_value(args.key, "TOS_KEY", f"Object Key(回车默认 {default_key})") or default_key
    endpoint = read_value(args.endpoint, "TOS_ENDPOINT", "Endpoint")
    region = read_value(args.region, "TOS_REGION", "Region")
    ak = read_value(args.ak, "TOS_ACCESS_KEY", "AccessKeyID")
    sk = read_value(args.sk, "TOS_SECRET_KEY", "AccessKeySecret", secret=True)
    token = read_value(args.token, "TOS_SECURITY_TOKEN", "SecurityToken(可留空)")

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

    presigned_url = client.generate_presigned_url(**kwargs)

    with open(file_path, "rb") as f:
        file_bytes = f.read()

    content_type = mimetypes.guess_type(file_path)[0] or "application/octet-stream"
    request = urllib.request.Request(
        presigned_url,
        data=file_bytes,
        method="PUT",
        headers={"Content-Type": content_type},
    )

    print(f"准备上传: {file_path}")
    print(f"对象路径: {key}")
    print(f"内容类型: {content_type}")
    print(f"文件大小: {len(file_bytes)} 字节")

    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            print(f"\n上传成功，HTTP 状态码: {response.status}")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        print(f"\n上传失败，HTTP 状态码: {e.code}", file=sys.stderr)
        print(body, file=sys.stderr)
        return 1
    except Exception as e:
        print(f"\n上传失败: {e}", file=sys.stderr)
        return 1

    print("可以去 TOS 控制台查看对象是否已经写入。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
