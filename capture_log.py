# -*- coding: utf-8 -*-
"""
串口报文抓取 - 自动处理 USB 掉线重连(设备重启循环时用)
用法: python capture_log.py [总秒数] [输出文件]
"""
import sys
import time
import glob
import serial
from serial.tools import list_ports

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "capture_log.txt"


def find_port():
    cands = []
    for p in list_ports.comports():
        if p.vid == 0x303A:
            cands.append(p.device)
    if cands:
        return cands[0]
    # 兜底: 重新枚举过的 com0com/USB 串口
    for d in glob.glob("COM*"):
        pass
    return None


start = time.time()
count = 0
total_lines = 0
f = open(OUT, "w", encoding="utf-8", errors="replace")

def emit(ts, text):
    global total_lines
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if not line.strip():
            continue
        out = "[%7.2f] %s\n" % (ts, line)
        sys.stdout.write(out)
        sys.stdout.flush()
        f.write(out)
        f.flush()
        total_lines += 1


port = None
ser = None
last_seen = None
while time.time() - start < SECS:
    elapsed = time.time() - start
    if ser is None:
        port = find_port()
        if port is None:
            time.sleep(0.3)
            continue
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = 115200
            ser.timeout = 0.2
            ser.dtr = True
            ser.rts = False
            ser.open()
            ser.reset_input_buffer()
            if last_seen is None:
                emit(elapsed, "=== 已连接 %s ===" % port)
            else:
                emit(elapsed, "=== 重新连接 %s ===" % port)
        except Exception as e:
            emit(elapsed, "打开 %s 失败: %r" % (port, e))
            ser = None
            time.sleep(0.5)
            continue
    try:
        data = ser.read(4096)
    except Exception as e:
        emit(elapsed, "=== 连接断开(设备复位): %s ===" % type(e).__name__)
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(0.5)
        continue
    if data:
        count += len(data)
        emit(elapsed, data.decode("utf-8", "replace"))
        last_seen = elapsed

if ser:
    try:
        ser.close()
    except Exception:
        pass
f.close()
print("=== 结束: %d 字节 / %d 行 -> %s ===" % (count, total_lines, OUT))
