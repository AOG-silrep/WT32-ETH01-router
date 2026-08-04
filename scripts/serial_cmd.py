#!/usr/bin/env python3
"""Send a command to a serial console and print what it says back.

The measurement gate drives two consoles over UART: this firmware's own
`aog-bridge>` REPL, and the ESP32 iperf peer that stands in for a WiFi client.
Neither is authenticated and both are line-oriented, so this is all the driver
they need - there is deliberately no session state, no expect language, and no
dependency beyond pyserial.

Opening the port resets the target, and there is no getting around it on these
boards: the CP2102N drives EN from RTS, so the line transition as the port comes
up pulls the chip into `rst:0x1 (POWERON_RESET)`. Clearing dtr/rts before open()
does not prevent it, and neither does `stty -hupcl` - both were tried. What
follows from that is the shape of this tool: **one open per session, many
commands**. Anything that needs several commands against one target must pass
them all to a single invocation, because each invocation costs a reboot.

Both lines are still cleared before open(), which costs nothing and keeps the
target out of download mode.

Usage:
    serial_cmd.py --port /dev/ttyUSB0 sysinfo
    serial_cmd.py --port /dev/ttyUSB2 --boot-wait 3 'sta_scan' 'iperf -s'
    serial_cmd.py --port /dev/ttyUSB0 --listen --read-secs 10
"""

import argparse
import sys
import time

import serial


def open_port(port, baud):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    # Short read timeout so the drain loop below is driven by its own deadline
    # rather than by whether the target happens to be talking.
    ser.timeout = 0.1
    # See the module docstring: cleared before open() so the target is not reset.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def drain(ser, secs):
    """Read for secs, returning whatever arrived. Never raises on partial UTF-8:
    a log line can be cut mid-character by the read window closing."""
    deadline = time.monotonic() + secs
    out = bytearray()
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            out.extend(chunk)
    return out.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="*",
                    help="commands to send in one session, in order; each is "
                         "given --read-secs to answer before the next is sent")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--read-secs", type=float, default=2.0,
                    help="how long to read after each command (default 2)")
    ap.add_argument("--settle-secs", type=float, default=0.3,
                    help="drained and discarded before the first command, so "
                         "output left over from boot is not attributed to it "
                         "(default 0.3)")
    ap.add_argument("--boot-wait", type=float, default=0.0,
                    help="seconds to wait after open before the first command, "
                         "for the reboot the open itself causes (see above)")
    ap.add_argument("--listen", action="store_true",
                    help="send nothing, just read (for capturing a boot log)")
    args = ap.parse_args()

    if not args.listen and not args.command:
        ap.error("give at least one command, or --listen to only read")

    try:
        ser = open_port(args.port, args.baud)
    except serial.SerialException as e:
        print(f"serial_cmd: cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    with ser:
        if args.listen:
            sys.stdout.write(drain(ser, args.read_secs))
            return 0

        if args.boot_wait:
            # Discarded, not printed: this is the reboot's own output, not an
            # answer to anything asked for here.
            drain(ser, args.boot_wait)
        drain(ser, args.settle_secs)

        for cmd in args.command:
            # Echoed so a transcript of several commands stays readable - the
            # targets' own prompts do not identify which output belongs to what.
            sys.stdout.write(f"\n===== {cmd} =====\n")
            ser.write((cmd + "\r\n").encode())
            ser.flush()
            sys.stdout.write(drain(ser, args.read_secs))
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
