#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""云STT客户端: 硅基流动 OpenAI兼容 audio/transcriptions
输入 16k mono int16 PCM, 返回识别文字
key来源: 参数 / 环境变量 SILICONFLOW_API_KEY / 同目录 stt_key.txt
"""
import io
import json
import os
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

BASE_URL = "https://api.siliconflow.cn/v1/audio/transcriptions"
DEFAULT_MODEL = "TeleAI/TeleSpeechASR"  # 实测~1秒; SenseVoiceSmall高峰排队严重仅备用


def get_siliconflow_key(api_key: str = None) -> str:
    key = api_key or os.environ.get("SILICONFLOW_API_KEY", "")
    if not key:
        kf = Path(__file__).parent / "stt_key.txt"
        if kf.exists():
            key = kf.read_text(encoding="utf-8").strip()
    if not key:
        raise RuntimeError("没有STT key: 参数传入, 或写进 stt_key.txt")
    return key


def pcm16_to_wav(pcm: bytes, rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


def transcribe_pcm(pcm: bytes, rate: int = 16000, key: str = None,
                   model: str = DEFAULT_MODEL, timeout: int = 30) -> str:
    """发一段PCM去云端识别, 返回文字"""
    if key is None:
        key = get_siliconflow_key()
    wav = pcm16_to_wav(pcm, rate)
    boundary = "----stt" + uuid.uuid4().hex
    body = bytearray()

    def field(name: str, value: str):
        body.extend(f"--{boundary}\r\n".encode())
        body.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        body.extend(f"{value}\r\n".encode())

    field("model", model)
    body.extend(f"--{boundary}\r\n".encode())
    body.extend(b'Content-Disposition: form-data; name="file"; filename="seg.wav"\r\n')
    body.extend(b"Content-Type: audio/wav\r\n\r\n")
    body.extend(wav)
    body.extend(f"\r\n--{boundary}--\r\n".encode())

    req = urllib.request.Request(BASE_URL, data=bytes(body), method="POST", headers={
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Authorization": f"Bearer {key}",
        "User-Agent": "esp32-voice-assistant/1.0",
    })
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 国内接口直连
    try:
        with opener.open(req, timeout=timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="ignore")[:300]
        hint = {401: "Key不对", 429: "限流"}.get(e.code, "")
        raise RuntimeError(f"STT HTTP {e.code} {hint} {detail}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"STT网络错误: {e.reason}")
    return (data.get("text") or "").strip()
