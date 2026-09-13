#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
ESP32 语音助手 全链路: 麦克风 -> STT -> 大模型 -> 晓晓TTS -> 喇叭
用法 (先烧录 esp32_voice.ino 二合一固件, USB CDC On Boot=启用):
  E:\faster-whisper\venv\Scripts\python.exe E:\opencode\esp32_voice_loop\pc_voice_assistant.py --port COM23
调试:
  # 不接麦, 只测 文字->大模型->喇叭
  ... pc_voice_assistant.py --say "给我讲一句话" --port COM23
  # 调门限(安静rms与说话rms之间), 默认400; 太灵敏就调高
  ... pc_voice_assistant.py --port COM23 --thresh 500
说明:
  - 半双工: 听的时候喇叭不喂数据=静音; 放的时候停麦克风流; 放完等1秒再听
  - 说一句、停一下(默认静音0.7s), 就识别+回答一句话
  - Ctrl+C 退出
"""
import argparse
import asyncio
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

import numpy as np

try:
    sys.stdout.reconfigure(line_buffering=True, errors="replace")
    sys.stderr.reconfigure(line_buffering=True, errors="replace")
except Exception:
    pass

sys.path.insert(0, str(Path(__file__).parent))
from llm_client import llm_chat
from stt_client import get_siliconflow_key, transcribe_pcm

SAMPLE_RATE = 16000
FRAME = 320          # 20ms一帧
PRE_ROLL = 16        # 预卷0.32秒
MIN_SPEECH = 25      # 最短0.5秒
MAX_SEG = 400        # 最长8秒强切
MODEL_DIR = r"E:\faster-whisper\models"

SYSTEM_PROMPT = ("你是一个语音助手，回答要口语化、简短，一般1到3句话，不超过80字，"
                 "不要用markdown、列表、代码块或表情符号。")

HALLUCINATIONS = {"谢谢观看", "谢谢大家观看", "请不吝点赞", "点赞订阅", "字幕由", "字幕组",
                  "明镜与点点栏目", "感谢观看", "下次再见"}


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


# ---------------- TTS (晓晓) ----------------

def synth_mp3(text: str, voice: str, rate: str) -> bytes:
    import edge_tts

    async def run() -> bytes:
        comm = edge_tts.Communicate(text, voice, rate=rate)
        buf = bytearray()
        async for chunk in comm.stream():
            if chunk["type"] == "audio":
                buf += chunk["data"]
        return bytes(buf)

    return asyncio.run(run())


def mp3_to_pcm16(mp3: bytes) -> bytes:
    import av

    tmp = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
    try:
        tmp.write(mp3)
        tmp.close()
        out = bytearray()
        with av.open(tmp.name) as c:
            rs = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)
            for frame in c.decode(audio=0):
                for f in rs.resample(frame):
                    out += f.to_ndarray().tobytes()
            for f in rs.resample(None):
                out += f.to_ndarray().tobytes()
        return bytes(out)
    finally:
        Path(tmp.name).unlink(missing_ok=True)


def clean_for_speech(t: str, maxlen: int) -> str:
    import re
    t = re.sub(r"```.*?```", "", t, flags=re.S)
    t = t.replace("**", "").replace("`", "").replace("#", "")
    t = re.sub(r"(?m)^\s*[-*+]\s+", "", t)
    t = re.sub(r"https?://\S+", "", t)
    t = re.sub(r"[ \t]+", " ", t).strip()
    if maxlen > 0 and len(t) > maxlen:
        cut = t[:maxlen]
        ends = [m.end() for m in re.finditer(r"[。！？!?.\n]", cut)]
        t = cut[:ends[-1]] if ends and ends[-1] >= maxlen * 0.5 else cut
    return t


# ---------------- 串口 (ESP32) ----------------

class EspLink:
    def __init__(self, port: str, baud: int):
        import serial
        print(f"[串口] 打开 {port} ...", flush=True)
        self.ser = serial.Serial(port, baud, timeout=2)
        time.sleep(0.5)
        self.ser.reset_input_buffer()
        self.ser.timeout = 0.3
        empty = 0
        while empty < 2:
            ln = self.ser.readline()
            if ln:
                empty = 0
                print(f"[ESP] {ln.decode(errors='ignore').strip()}", flush=True)
            else:
                empty += 1
        # 固件探测: 二合一固件对未知命令回 'ERR cmd, use R<sec>/S/Q/A<len>/M0/M1'
        self.ser.timeout = 2
        self.ser.write(b"P\n")
        self.ser.flush()
        resp = self.ser.readline().decode(errors="ignore").strip()
        if "A<len>" not in resp:
            raise RuntimeError(
                f"板子里不是二合一固件 (探测回复 {resp!r})\n"
                f"  请烧录 E:\\opencode\\esp32_voice_loop\\esp32_voice\\esp32_voice.ino\n"
                f"  Arduino工具里 USB CDC On Boot 必须=启用"
            )
        print(f"[ESP] 固件OK: {resp!r}", flush=True)

    def start_stream(self) -> None:
        self.ser.reset_input_buffer()
        self.ser.timeout = 3
        self.ser.write(b"S\n")
        self.ser.flush()
        line = self.ser.readline().decode(errors="ignore").strip()
        if "STREAM" not in line:
            raise RuntimeError(f"开始流失败: {line!r}")

    def stop_stream(self, timeout: float = 3.0) -> None:
        """发Q并吃掉 STREAM 尾巴直到 STOP 标记"""
        self.ser.write(b"Q\n")
        self.ser.flush()
        buf = bytearray()
        end = time.time() + timeout
        self.ser.timeout = 0.2
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf.extend(chunk)
                if b"STOP" in buf:
                    break

    def play(self, pcm: bytes) -> None:
        s = self.ser
        s.timeout = 5
        s.write(f"A{len(pcm)}\n".encode())
        s.flush()
        line = s.readline().decode(errors="ignore").strip()
        if "OK" not in line:
            raise RuntimeError(f"播放未就绪: {line!r}")
        CHUNK = 4096
        for off in range(0, len(pcm), CHUNK):
            s.write(pcm[off:off + CHUNK])
            s.flush()
            ack = s.read(1)
            if ack != b"K":
                tail = s.readline().decode(errors="ignore").strip()
                raise RuntimeError(f"流控失败 ack={ack!r} {tail}")
        line = s.readline().decode(errors="ignore").strip()
        if "DONE" not in line:
            raise RuntimeError(f"播放未确认: {line!r}")

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass


# ---------------- 主流程 ----------------

def load_stt(model_name: str):
    from faster_whisper import WhisperModel
    print(f"[STT] 加载 {model_name} ...", flush=True)
    m = WhisperModel(model_name, device="cpu", compute_type="int8", download_root=MODEL_DIR)
    print("[STT] 就绪", flush=True)
    return m


def transcribe(model, seg: np.ndarray, lang) -> str:
    audio = seg.astype(np.float32) / 32768.0
    segments, info = model.transcribe(audio, language=lang, beam_size=5,
                                      vad_filter=True,
                                      vad_parameters=dict(min_silence_duration_ms=300))
    return "".join(s.text for s in segments).strip()


def listen_one_sentence(esp: EspLink, args) -> np.ndarray:
    """开流 -> VAD切一句 -> 停流, 返回这句PCM"""
    esp.start_stream()
    print(f"[听] 说话吧... (thresh={args.thresh})", flush=True)
    sil_frames = int(args.silence * SAMPLE_RATE / FRAME)
    buf = bytearray()
    pre = []
    recording = []
    trig = 0
    sil = 0
    idle_max = 0.0
    last_meter = time.time()
    esp.ser.timeout = 2

    while True:
        need = FRAME * 2 - len(buf)
        chunk = esp.ser.read(max(need, 4096))
        if chunk:
            buf.extend(chunk)
        while len(buf) >= FRAME * 2:
            frame = np.frombuffer(bytes(buf[:FRAME * 2]), dtype=np.int16)
            del buf[:FRAME * 2]
            v = rms(frame)
            if not recording:
                idle_max = max(idle_max, v)
                pre.append(frame)
                if len(pre) > PRE_ROLL:
                    pre.pop(0)
                trig = trig + 1 if v > args.thresh else 0
                if trig >= 3:  # 连续3帧超门限才认为开口
                    recording = pre.copy()
                    pre.clear()
                    trig = 0
                    sil = 0
            else:
                recording.append(frame)
                n = len(recording)
                if v > args.thresh:
                    sil = 0
                else:
                    sil += 1
                if (sil >= sil_frames and n >= MIN_SPEECH) or n >= MAX_SEG:
                    seg = np.concatenate(recording)
                    esp.stop_stream()
                    return seg
        if time.time() - last_meter > 3 and not recording:
            print(f"[电平] 安静rms~{idle_max:.0f} (说话时该明显高于它)", flush=True)
            idle_max = 0.0
            last_meter = time.time()


def main() -> None:
    ap = argparse.ArgumentParser(description="ESP32语音助手: 麦->STT->大模型->喇叭")
    ap.add_argument("--port", default="COM23")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--stt", default="cloud", choices=["cloud", "local"],
                    help="cloud=硅基流动云识别(默认,约1秒); local=本机whisper")
    ap.add_argument("--stt-model", default="TeleAI/TeleSpeechASR",
                    help="云STT模型 (备用: FunAudioLLM/SenseVoiceSmall 会排队)")
    ap.add_argument("--model", default="small", help="local模式用的whisper模型")
    ap.add_argument("--lang", default="zh")
    ap.add_argument("--voice", default="zh-CN-XiaoxiaoNeural")
    ap.add_argument("--rate", default="+0%")
    ap.add_argument("--thresh", type=float, default=400, help="能量门限")
    ap.add_argument("--silence", type=float, default=0.7, help="静音多久算说完")
    ap.add_argument("--maxlen", type=int, default=150, help="回复最长朗读字数")
    ap.add_argument("--provider", default="opencode-go")
    ap.add_argument("--llm-model", default="", help="不填用默认")
    ap.add_argument("--history", type=int, default=2, help="带几轮上下文, 0=不带")
    ap.add_argument("--llm-system", default=SYSTEM_PROMPT)
    ap.add_argument("--say", default="", help="单次测试: 把这句文字发给大模型, 朗读后退出")
    ap.add_argument("--text-chat", action="store_true", help="打字聊天模式: 输入文字->大模型->喇叭")
    args = ap.parse_args()

    esp = EspLink(args.port, args.baud)
    llm_model = args.llm_model or None
    history = []

    def ask_and_play(text: str) -> None:
        reply = llm_chat(text, provider=args.provider, model=llm_model,
                         system=args.llm_system,
                         history=history[-args.history * 2:] if args.history else None)
        print(f"AI> {reply}", flush=True)
        if args.history:
            history.append({"role": "user", "content": text})
            history.append({"role": "assistant", "content": reply})
        tts = clean_for_speech(reply, args.maxlen)
        if not tts:
            return
        pcm = mp3_to_pcm16(synth_mp3(tts, args.voice, args.rate))
        print(f"[播放] {len(pcm)/2/SAMPLE_RATE:.1f}s ...", flush=True)
        esp.play(pcm)
        time.sleep(0.8)

    try:
        if args.say:
            print(f"你> {args.say}", flush=True)
            ask_and_play(args.say)
            return

        if args.text_chat:
            while True:
                try:
                    text = input("你> ").strip()
                except EOFError:
                    break
                if not text:
                    break
                try:
                    ask_and_play(text)
                except Exception as e:
                    print(f"[出错] {e}", flush=True)
            return

        use_cloud = args.stt == "cloud"
        if use_cloud:
            stt_key = get_siliconflow_key()
            print("[STT] 云识别(硅基流动)", flush=True)
        else:
            model = load_stt(args.model)
            lang = None if args.lang == "auto" else args.lang
        print(f"[就绪] 音色{args.voice}, 大模型{args.provider}/{llm_model or '默认'}", flush=True)

        while True:
            seg = listen_one_sentence(esp, args)
            t0 = time.time()
            if use_cloud:
                text = transcribe_pcm(seg.tobytes(), rate=SAMPLE_RATE,
                                      key=stt_key, model=args.stt_model)
            else:
                text = transcribe(model, seg, lang)
            text = text.strip()
            if not text or len(text) < 2 or any(h in text for h in HALLUCINATIONS):
                print(f"[识别] (没听清, 继续)", flush=True)
                time.sleep(0.2)
                continue
            print(f"[识别] {text}  ({len(seg)/SAMPLE_RATE:.1f}s音频)", flush=True)

            try:
                reply = llm_chat(text, provider=args.provider, model=llm_model,
                                 system=args.llm_system,
                                 history=history[-args.history * 2:] if args.history else None)
            except Exception as e:
                print(f"[大模型] 出错: {e}", flush=True)
                continue
            print(f"[大模型] {reply}", flush=True)

            if args.history:
                history.append({"role": "user", "content": text})
                history.append({"role": "assistant", "content": reply})

            tts = clean_for_speech(reply, args.maxlen)
            if not tts:
                continue
            pcm = mp3_to_pcm16(synth_mp3(tts, args.voice, args.rate))
            print(f"[播放] {len(pcm)/2/SAMPLE_RATE:.1f}s (整轮{time.time()-t0:.1f}s)", flush=True)
            esp.play(pcm)
            time.sleep(1.0)  # 等喇叭余音散掉再开麦
            print("", flush=True)

    except KeyboardInterrupt:
        print("\n[退出] Ctrl+C", flush=True)
        try:
            esp.stop_stream(timeout=1)
        except Exception:
            pass
    finally:
        esp.close()


if __name__ == "__main__":
    main()
