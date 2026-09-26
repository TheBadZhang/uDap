#!/usr/bin/env python3
"""Verify UART-to-CDC transfers sent as strictly acknowledged bursts."""

import argparse
import os
import select
import sys
import termios
import time

from usb_cdc_loopback_test import configure, payload


def read_and_check(fd: int, received: bytearray, expected: bytes) -> None:
    data = os.read(fd, min(4096, len(expected) - len(received)))
    if not data:
        return
    begin = len(received)
    received.extend(data)
    actual = received[begin:]
    wanted = expected[begin:len(received)]
    if actual != wanted:
        offset = next(i for i, pair in enumerate(zip(actual, wanted)) if pair[0] != pair[1])
        raise ValueError(
            f"mismatch at {begin + offset}: got=0x{actual[offset]:02x} "
            f"expected=0x{wanted[offset]:02x}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", help="external USB-UART source tty")
    parser.add_argument("cdc", help="CH32 CDC tty receiving the bytes")
    parser.add_argument("--baud", type=int, default=1000000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--burst-size", type=int, default=4096)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    if args.bytes <= 0 or args.burst_size <= 0 or args.bytes % args.burst_size or args.timeout <= 0:
        parser.error("bytes must be a positive multiple of burst-size; timeout must be positive")

    source_fd = os.open(args.source, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    cdc_fd = os.open(args.cdc, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(source_fd, args.baud)
        configure(cdc_fd, args.baud)
        time.sleep(0.1)
        termios.tcflush(source_fd, termios.TCIOFLUSH)
        termios.tcflush(cdc_fd, termios.TCIOFLUSH)

        expected = payload(args.bytes, 0x53535258)
        received = bytearray()
        deadline = time.monotonic() + args.timeout
        started = time.monotonic()

        for begin in range(0, len(expected), args.burst_size):
            end = begin + args.burst_size
            written = begin
            while written < end:
                if time.monotonic() > deadline:
                    raise TimeoutError(f"timeout writing burst at {begin}, received={len(received)}")
                readable, writable, _ = select.select([cdc_fd], [source_fd], [], 0.1)
                if cdc_fd in readable:
                    read_and_check(cdc_fd, received, expected)
                if source_fd in writable:
                    written += os.write(source_fd, expected[written:end])

            while len(received) < end:
                if time.monotonic() > deadline:
                    raise TimeoutError(f"timeout receiving burst at {begin}, received={len(received)}")
                readable, _, _ = select.select([cdc_fd], [], [], 0.1)
                if cdc_fd in readable:
                    read_and_check(cdc_fd, received, expected)

        elapsed = time.monotonic() - started
        print(
            f"PASS {len(expected)} bytes in {elapsed:.3f}s "
            f"({len(expected) / elapsed / 1024:.1f} KiB/s), "
            f"{args.burst_size}-byte acknowledged bursts"
        )
        return 0
    finally:
        os.close(source_fd)
        os.close(cdc_fd)


if __name__ == "__main__":
    sys.exit(main())
