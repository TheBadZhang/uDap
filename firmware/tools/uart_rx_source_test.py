#!/usr/bin/env python3
"""Verify an external USB-UART TX path through one CDC UART RX."""

import argparse
import os
import select
import sys
import termios
import time

from usb_cdc_loopback_test import configure, payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", help="external USB-UART source tty")
    parser.add_argument("cdc", help="CH32 CDC tty receiving the bytes")
    parser.add_argument("--baud", type=int, default=3000000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--write-chunk", type=int, default=251)
    parser.add_argument("--window", type=int, default=8192)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    if args.bytes <= 0 or args.write_chunk <= 0 or args.window <= 0:
        parser.error("bytes, write-chunk, and window must be positive")

    source_fd = os.open(args.source, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    cdc_fd = os.open(args.cdc, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(source_fd, args.baud)
        configure(cdc_fd, args.baud)
        time.sleep(0.1)
        termios.tcflush(source_fd, termios.TCIOFLUSH)
        termios.tcflush(cdc_fd, termios.TCIOFLUSH)

        expected = payload(args.bytes, 0x6D2B79F5 ^ 1)
        received = bytearray()
        sent = 0
        deadline = time.monotonic() + args.timeout
        started = time.monotonic()

        while len(received) < len(expected):
            if time.monotonic() > deadline:
                raise TimeoutError(f"timeout sent={sent} received={len(received)}")
            can_write = sent < len(expected) and sent - len(received) < args.window
            readable, writable, _ = select.select(
                [cdc_fd], [source_fd] if can_write else [], [], 0.1
            )
            if cdc_fd in readable:
                data = os.read(cdc_fd, min(4096, len(expected) - len(received)))
                if data:
                    begin = len(received)
                    received.extend(data)
                    if received[begin:] != expected[begin:len(received)]:
                        for offset, actual in enumerate(received[begin:]):
                            expected_byte = expected[begin + offset]
                            if actual != expected_byte:
                                raise ValueError(
                                    f"mismatch at {begin + offset}: got=0x{actual:02x} "
                                    f"expected=0x{expected_byte:02x}"
                                )
            if source_fd in writable:
                sent += os.write(source_fd, expected[sent:sent + args.write_chunk])

        elapsed = time.monotonic() - started
        print(f"PASS {len(expected)} bytes in {elapsed:.3f}s ({len(expected) / elapsed / 1024:.1f} KiB/s)")
        return 0
    finally:
        os.close(source_fd)
        os.close(cdc_fd)


if __name__ == "__main__":
    sys.exit(main())
