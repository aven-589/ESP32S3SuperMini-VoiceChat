#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
大模型连通性测试: 发一句话, 看回复
用法:
  # opencode-go 订阅 (默认, key自动从本机opencode读, 需开着代理)
  E:\faster-whisper\venv\Scripts\python.exe E:\opencode\esp32_voice_loop\pc_llm_test.py

  # 换问题 / 换模型 (可选: deepseek-v4-flash/glm-5.3-flash/qwen3.8-flash/...)
  ... pc_llm_test.py --msg "用一句话讲个冷笑话"
  ... pc_llm_test.py --model deepseek-v4-flash

  # 也可以用别的厂商(deepseek/siliconflow/zhipu/moonshot/qwen), 需自己给key
  ... pc_llm_test.py --provider deepseek --api-key sk-xxxxxx
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from llm_client import PROVIDERS, llm_chat


def main() -> None:
    ap = argparse.ArgumentParser(description="大模型连通性测试")
    ap.add_argument("--provider", default="opencode-go", choices=list(PROVIDERS.keys()))
    ap.add_argument("--api-key", default="", help="不传则读环境变量")
    ap.add_argument("--model", default="", help="不传则用该家默认模型")
    ap.add_argument("--msg", default="你好，请用一句话介绍一下你自己")
    ap.add_argument("--system", default="", help="可选系统提示词")
    args = ap.parse_args()

    print(f"[测试] {args.provider} -> {args.model or PROVIDERS[args.provider]['model']}")
    print(f"[发送] {args.msg}")
    t0 = time.time()
    try:
        reply = llm_chat(args.msg, provider=args.provider,
                         api_key=args.api_key or None,
                         model=args.model or None,
                         system=args.system or None)
    except Exception as e:
        print(f"[失败] {e}")
        sys.exit(1)
    dt = time.time() - t0
    print(f"[回复] 耗时{dt:.1f}s")
    print(reply)
    print("[OK] 大模型通信正常")


if __name__ == "__main__":
    main()
