"""Drive the WonderScope serial console from the PC (smoke test / scripting example).

usage: python tools/serial_console_test.py COM13 "help" "status" ...
Lines starting with '{' are sent as JSON requests (machine interface).
"""
import sys
import time

import serial


def read_for(ser, seconds):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            end = max(end, time.time() + 0.4)  # keep reading while data flows
    return buf.decode("utf-8", "replace")


def main():
    port = sys.argv[1]
    cmds = sys.argv[2:]
    ser = serial.Serial(port, 115200, timeout=0.1)
    ser.dtr = True
    print(read_for(ser, 2.5), end="")
    for c in cmds:
        wait = 1.5
        if c.startswith("wait:"):
            time.sleep(float(c[5:]))
            continue
        if c.startswith("slow:"):
            c, wait = c[5:], 12
        ser.write((c + "\r\n").encode())
        out = read_for(ser, wait)
        print(out, end="")
    print()


if __name__ == "__main__":
    main()
