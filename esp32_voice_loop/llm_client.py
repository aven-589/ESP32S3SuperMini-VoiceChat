#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""大模型客户端 (OpenAI兼容接口, 国内直连不走系统代理)
语音助手和测试脚本共用
"""
import json
import os
import urllib.error
import urllib.request
import uuid

# opencode-go 要求: 稳定会话ID(同一进程内复用, 用于路由和缓存)
OPENCODE_SESSION_ID = os.environ.get("OPENCODE_SESSION_ID") or str(uuid.uuid4())

PROVIDERS = {
    "opencode-go": {
        # opencode 订阅网关 (OpenAI兼容), key自动从本机 opencode auth.json 读
        "base_url": "https://opencode.ai/zen/go/v1/chat/completions",
        "model": "deepseek-v4.1-flash",
        "env": "OPENCODE_API_KEY",
        "auth_file": "opencode-go",  # auth.json里的字段名
        "use_proxy": True,           # 境外接口, 走环境变量里的代理
    },
    "deepseek": {
        "base_url": "https://api.deepseek.com/v1/chat/completions",
        "model": "deepseek-chat",
        "env": "DEEPSEEK_API_KEY",
    },
    "siliconflow": {
        "base_url": "https://api.siliconflow.cn/v1/chat/completions",
        "model": "Qwen/Qwen2.5-7B-Instruct",
        "env": "SILICONFLOW_API_KEY",
    },
    "zhipu": {
        "base_url": "https://open.bigmodel.cn/api/paas/v4/chat/completions",
        "model": "glm-4-flash",
        "env": "ZHIPUAI_API_KEY",
    },
    "moonshot": {
        "base_url": "https://api.moonshot.cn/v1/chat/completions",
        "model": "moonshot-v1-8k",
        "env": "MOONSHOT_API_KEY",
    },
    "qwen": {
        "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        "model": "qwen-turbo",
        "env": "DASHSCOPE_API_KEY",
    },
}


def _key_from_opencode_auth(field: str) -> str:
    """从本机 opencode 的 auth.json 读 key (路径: ~/.local/share/opencode/auth.json)"""
    p = os.path.join(os.path.expanduser("~"), ".local", "share", "opencode", "auth.json")
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        return str(data.get(field, {}).get("key", "") or "")
    except Exception:
        return ""


def get_api_key(provider: str, api_key: str = None) -> str:
    p = PROVIDERS[provider]
    key = api_key or os.environ.get(p["env"], "")
    if not key and p.get("auth_file"):
        key = _key_from_opencode_auth(p["auth_file"])
    if not key:
        raise RuntimeError(f"没有API Key: 设置环境变量 {p['env']}, 或用 --api-key 传入")
    return key


def llm_chat(text, provider="deepseek", api_key=None, model=None, system=None,
             history=None, timeout=60):
    """发一句用户消息, 返回助手回复文本
    history: 可选 [{"role":..., "content":...}, ...] 前缀历史
    """
    p = PROVIDERS[provider]
    key = get_api_key(provider, api_key)
    model = model or p["model"]
    messages = []
    if system:
        messages.append({"role": "system", "content": system})
    if history:
        messages.extend(history)
    messages.append({"role": "user", "content": text})

    body = json.dumps({"model": model, "messages": messages, "stream": False}).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",
        "User-Agent": "esp32-voice-assistant/1.0",
    }
    if provider == "opencode-go":
        headers["x-opencode-session"] = OPENCODE_SESSION_ID
    req = urllib.request.Request(p["base_url"], data=body, method="POST", headers=headers)
    if p.get("use_proxy"):
        opener = urllib.request.build_opener()  # 境外接口: 跟随环境变量代理
    else:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 国内接口直连
    try:
        with opener.open(req, timeout=timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="ignore")[:300]
        hint = {401: "Key不对或没开通", 402: "余额不足", 429: "限流, 稍后再试"}.get(e.code, "")
        raise RuntimeError(f"HTTP {e.code} {hint} {detail}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"网络错误: {e.reason} (检查网络/防火墙)")
    try:
        return data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError):
        raise RuntimeError(f"返回格式异常: {json.dumps(data, ensure_ascii=False)[:300]}")
