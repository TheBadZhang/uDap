#!/usr/bin/env python3
"""Verify one CDC-to-UART TX path with an external USB-UART receiver."""

import argparse
import os
import select
import sys
import termios
import time

from usb_cdc_loopback_test import configure, payload


def drain(fd: int) -> None:
    deadline = time.monotonic() + 0.25
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.01)
        if not readable:
            continue
        try:
            if not os.read(fd, 4096):
                return
        except BlockingIOError:
            return


def mismatch_error(received: bytearray, expected: bytes, position: int, context: int) -> ValueError:
    analysis_end = min(len(received), position + context + 1)
    needle_len = min(16, analysis_end - position)
    prior = -1
    resume = -1
    if needle_len > 0:
        prior = expected.rfind(
            received[position:position + needle_len],
            max(0, position - 2048),
            position,
        )
        resume = received.find(
            expected[position:position + needle_len],
            position + 1,
            analysis_end,
        )
    display_context = min(context, 64)
    start = max(0, position - display_context)
    end = min(len(received), position + display_context + 1)
    return ValueError(
        f"mismatch at {position}: got=0x{received[position]:02x} "
        f"expected=0x{expected[position]:02x}; prior_delta={prior - position if prior >= 0 else None} "
        f"resume_after={resume - position if resume >= 0 else None}; "
        f"got={received[start:end].hex()} "
        f"expected={expected[start:end].hex()}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cdc", help="CH32 CDC tty to transmit through")
    parser.add_argument("receiver", help="external USB-UART receiver tty")
    parser.add_argument("--baud", type=int, default=3000000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--write-chunk", type=int, default=251)
    parser.add_argument("--window", type=int, default=8192)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--hold-on-failure", type=float, default=0.0)
    parser.add_argument("--mismatch-context", type=int, default=0)
    args = parser.parse_args()

    if args.bytes <= 0 or args.write_chunk <= 0 or args.window <= 0:
        parser.error("bytes, write-chunk, and window must be positive")

    source_fd = os.open(args.cdc, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    receiver_fd = os.open(args.receiver, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(source_fd, args.baud)
        configure(receiver_fd, args.baud)
        time.sleep(0.1)
        termios.tcflush(source_fd, termios.TCIOFLUSH)
        termios.tcflush(receiver_fd, termios.TCIOFLUSH)
        drain(receiver_fd)

        expected = payload(args.bytes, 0x6D2B79F5)
        received = bytearray()
        sent = 0
        mismatch_at = None
        started = time.monotonic()
        deadline = started + args.timeout

        while len(received) < len(expected):
            if time.monotonic() > deadline:
                if mismatch_at is not None:
                    raise mismatch_error(received, expected, mismatch_at, args.mismatch_context)
                raise TimeoutError(f"timeout sent={sent} received={len(received)}")

            can_write = mismatch_at is None and sent < len(expected) and sent - len(received) < args.window
            readable, writable, _ = select.select(
                [receiver_fd], [source_fd] if can_write else [], [], 0.1
            )
            if receiver_fd in readable:
                data = os.read(receiver_fd, min(4096, len(expected) - len(received)))
                if data:
                    begin = len(received)
                    received.extend(data)
                    if mismatch_at is None and received[begin:] != expected[begin:len(received)]:
                        for offset, actual in enumerate(received[begin:]):
                            expected_byte = expected[begin + offset]
                            if actual != expected_byte:
                                mismatch_at = begin + offset
                                break
                    if mismatch_at is not None and (
                        args.mismatch_context == 0
                        or len(received) >= mismatch_at + args.mismatch_context + 1
                    ):
                        raise mismatch_error(received, expected, mismatch_at, args.mismatch_context)
            if source_fd in writable:
                sent += os.write(source_fd, expected[sent:sent + args.write_chunk])

        elapsed = time.monotonic() - started
        print(f"PASS {len(expected)} bytes in {elapsed:.3f}s ({len(expected) / elapsed / 1024:.1f} KiB/s)")
        return 0
    except Exception:
        if args.hold_on_failure > 0.0:
            print(f"holding ports open for {args.hold_on_failure:.1f}s", flush=True)
            time.sleep(args.hold_on_failure)
        raise
    finally:
        os.close(source_fd)
        os.close(receiver_fd)


if __name__ == "__main__":
    sys.exit(main())
