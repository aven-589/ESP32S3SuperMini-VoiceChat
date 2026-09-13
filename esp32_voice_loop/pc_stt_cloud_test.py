#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
云STT测试: 把本地wav发到 硅基流动 识别, 对比用
用法:
  E:\faster-whisper\venv\Scripts\python.exe E:\opencode\esp32_voice_loop\pc_stt_cloud_test.py --wav E:\opencode\esp32_voice_loop\test_zh.wav
参数:
  --wav    要识别的wav文件 (默认 test_zh.wav)
  --model  默认 TeleAI/TeleSpeechASR (实测~1秒不排队);
           FunAudioLLM/SenseVoiceSmall 识别准但高峰排队严重(0.6~165s), 仅备用
  key来源: --api-key / 环境变量 SILICONFLOW_API_KEY / 同目录 stt_key.txt
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

try:
    sys.stdout.reconfigure(line_buffering=True, errors="replace")
except Exception:
    pass

BASE_URL = "https://api.siliconflow.cn/v1/audio/transcriptions"
DEFAULT_MODEL = "TeleAI/TeleSpeechASR"


def get_key(args) -> str:
    key = args.api_key or os.environ.get("SILICONFLOW_API_KEY", "")
    if not key:
        kf = Path(__file__).parent / "stt_key.txt"
        if kf.exists():
            key = kf.read_text(encoding="utf-8").strip()
    if not key:
        print("[错误] 没找到key: 用 --api-key 传, 或写进 stt_key.txt")
        sys.exit(2)
    return key


def wav_duration(path: Path) -> float:
    try:
        with wave.open(str(path), "rb") as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return 0.0


def transcribe(path: Path, key: str, model: str, timeout: int = 60) -> dict:
    boundary = "----stt" + uuid.uuid4().hex
    body = bytearray()

    def field(name: str, value: str):
        body.extend(f"--{boundary}\r\n".encode())
        body.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        body.extend(f"{value}\r\n".encode())

    field("model", model)
    body.extend(f"--{boundary}\r\n".encode())
    body.extend(f'Content-Disposition: form-data; name="file"; filename="{path.name}"\r\n'.encode())
    body.extend(b"Content-Type: audio/wav\r\n\r\n")
    body.extend(path.read_bytes())
    body.extend(f"\r\n--{boundary}--\r\n".encode())

    req = urllib.request.Request(BASE_URL, data=bytes(body), method="POST", headers={
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Authorization": f"Bearer {key}",
        "User-Agent": "esp32-voice-assistant/1.0",
    })
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 国内接口直连
    try:
        with opener.open(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="ignore")[:400]
        hint = {401: "Key不对", 403: "没权限/未实名", 429: "限流"}.get(e.code, "")
        raise RuntimeError(f"HTTP {e.code} {hint} {detail}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"网络错误: {e.reason}")


def main() -> None:
    ap = argparse.ArgumentParser(description="云STT测试(硅基流动 SenseVoiceSmall)")
    ap.add_argument("--wav", default=str(Path(__file__).parent / "test_zh.wav"))
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--api-key", default="")
    args = ap.parse_args()

    path = Path(args.wav)
    if not path.exists():
        print(f"[错误] 找不到音频: {path}")
        sys.exit(2)
    key = get_key(args)

    dur = wav_duration(path)
    print(f"[音频] {path.name}, {dur:.1f}秒, {path.stat().st_size/1024:.0f}KB")
    print(f"[模型] {args.model}")

    t0 = time.time()
    try:
        data = transcribe(path, key, args.model)
    except Exception as e:
        print(f"[失败] {e}")
        sys.exit(1)
    dt = time.time() - t0

    text = (data.get("text") or "").strip()
    print(f"[识别] 耗时{dt:.2f}s")
    print(f"[结果] {text}")
    if not text:
        print(f"[原始返回] {json.dumps(data, ensure_ascii=False)[:400]}")
    else:
        print("[OK] 云STT通信正常")


if __name__ == "__main__":
    main()
