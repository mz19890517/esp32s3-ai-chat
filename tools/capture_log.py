#!/usr/bin/env python3
import argparse
import datetime
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺少 pyserial，先执行: pip install pyserial")

ESP_VID = 0x303A
USB_UART_VIDS = (0x1A86, 0x10C4, 0x0403, 0x1B8F)


def now():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def find_port():
    esp, other = [], []
    for p in list_ports.comports():
        if p.vid is None:
            continue
        if p.vid == ESP_VID:
            esp.append(p.device)
        elif p.vid in USB_UART_VIDS:
            other.append(p.device)
    return (esp + other + [None])[0]


def open_port(port, baud):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser(description="ESP32 自动重连串口抓取")
    ap.add_argument("port", nargs="?", help="串口名，如 COM5 或 /dev/ttyUSB0，留空自动识别")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-o", "--out", default="esp_boot.log")
    ap.add_argument("-q", "--quiet", action="store_true", help="只写文件，不在终端回显")
    args = ap.parse_args()

    target = args.port
    print(f"输出: {args.out}   波特率: {args.baud}")
    print("请先关掉浏览器里占用串口的连接，Ctrl+C 结束\n")

    with open(args.out, "a", encoding="utf-8", errors="replace") as f:
        ser = None

        def emit(text):
            f.write(text)
            f.flush()
            if not args.quiet:
                sys.stdout.write(text)
                sys.stdout.flush()

        def mark(text):
            line = f"\n--- {now()} {text} ---\n"
            f.write(line)
            f.flush()
            if not args.quiet:
                sys.stdout.write(line)
                sys.stdout.flush()

        try:
            while True:
                if ser is None:
                    port = target or find_port()
                    if not port:
                        time.sleep(1)
                        continue
                    try:
                        ser = open_port(port, args.baud)
                    except Exception:
                        time.sleep(0.5)
                        continue
                    mark(f"CONNECT {port}")
                    continue

                try:
                    data = ser.read(4096)
                except Exception:
                    data = None

                if data is None:
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    mark("DISCONNECT")
                    time.sleep(0.3)
                    continue

                if data:
                    emit(data.decode("utf-8", "replace"))
        except KeyboardInterrupt:
            print(f"\n已结束，日志写入 {args.out}")
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass


if __name__ == "__main__":
    main()
