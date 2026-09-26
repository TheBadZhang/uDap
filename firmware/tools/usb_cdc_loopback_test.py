#!/usr/bin/env python3
"""Strict byte-order loopback test for the CH32X035 CDC test firmware."""

import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import threading
import time


def payload(size: int, seed: int) -> bytes:
    value = seed
    data = bytearray(size)
    for index in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[index] = value & 0xFF
    return bytes(data)


IOSSIOSPEED = 0x80085402


def configure(fd: int, baud: int) -> None:
    attributes = termios.tcgetattr(fd)
    attributes[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                       termios.ISTRIP | termios.INLCR | termios.IGNCR |
                       termios.ICRNL | termios.IXON | termios.IXOFF | termios.IXANY)
    attributes[1] &= ~termios.OPOST
    attributes[2] |= termios.CS8
    attributes[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    standard_speed = getattr(termios, f"B{baud}", termios.B115200)
    attributes[4] = standard_speed
    attributes[5] = standard_speed
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)
    if standard_speed == termios.B115200 and baud != 115200:
        if sys.platform != "darwin":
            raise ValueError(f"non-standard baud {baud} is only supported on macOS")
        fcntl.ioctl(fd, IOSSIOSPEED, struct.pack("L", baud))
    termios.tcflush(fd, termios.TCIOFLUSH)


def drain(fd: int) -> None:
    """Discard bytes already queued by the USB serial driver."""
    deadline = time.monotonic() + 0.25
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.01)
        if not readable:
            continue
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break


def run_port(path: str, count: int, seed: int, baud: int, write_chunk: int, window: int, out_only: bool, timeout: float, results: dict) -> None:
    expected = payload(count, seed)
    fd = None
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure(fd, baud)
        # macOS may issue several asynchronous SET_LINE_CODING requests while
        # applying termios. Let the device finish reconfiguring its UART before
        # the measured byte stream starts.
        time.sleep(0.1)
        drain(fd)
        if write_chunk == 0 or out_only:
            os.write(fd, b"\xa5")
            if out_only:
                time.sleep(0.05)
        sent = 0
        received = bytearray()
        deadline = time.monotonic() + timeout
        started = time.monotonic()
        while len(received) < count:
            if time.monotonic() > deadline:
                raise TimeoutError(f"timeout sent={sent} received={len(received)}")
            can_write = write_chunk and sent < count and (out_only or sent - len(received) < window)
            readable, writable, _ = select.select([fd], [fd] if can_write else [], [], 0.1)
            if readable:
                try:
                    data = os.read(fd, min(4096, count - len(received)))
                except BlockingIOError:
                    data = b""
                if data:
                    if out_only:
                        received.extend(data)
                        if len(received) >= 4:
                            if received[:4] != b"PASS":
                                raise ValueError(f"device reported {received[:4]!r}")
                            elapsed = time.monotonic() - started
                            results[path] = f"PASS {count} bytes in {elapsed:.3f}s ({count / elapsed / 1024:.1f} KiB/s)"
                            return
                        continue
                    received.extend(data)
                    begin = len(received) - len(data)
                    if received[begin:] != expected[begin:len(received)]:
                        for offset, actual in enumerate(received[begin:]):
                            expected_byte = expected[begin + offset]
                            if actual != expected_byte:
                                start = max(0, begin + offset - 8)
                                end = min(len(received), begin + offset + 24)
                                raise ValueError(
                                    f"mismatch at {begin + offset}: got=0x{actual:02x} expected=0x{expected_byte:02x}; "
                                    f"got={received[start:end].hex()} expected={expected[start:end].hex()}"
                                )
            if writable:
                chunk = expected[sent:min(sent + write_chunk, count)]
                try:
                    sent += os.write(fd, chunk)
                except BlockingIOError:
                    pass
        elapsed = time.monotonic() - started
        results[path] = f"PASS {count} bytes in {elapsed:.3f}s ({count / elapsed / 1024:.1f} KiB/s)"
    except Exception as exc:  # report every port result to the parent
        results[path] = f"FAIL {exc}"
    finally:
        if fd is not None:
            os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="+", help="CDC tty paths")
    parser.add_argument("--bytes", type=int, default=64 * 1024)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--write-chunk", type=int, default=251)
    parser.add_argument("--window", type=int, default=1024, help="maximum sent but not echoed bytes per port")
    parser.add_argument("--timeout", type=float, default=0.0, help="per-port timeout in seconds")
    parser.add_argument("--read-only", action="store_true", help="read the firmware's deterministic test stream")
    parser.add_argument("--out-only", action="store_true", help="write deterministic data for firmware-side OUT validation")
    args = parser.parse_args()
    if args.bytes <= 0:
        parser.error("--bytes must be positive")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.write_chunk <= 0:
        parser.error("--write-chunk must be positive")
    if args.window <= 0:
        parser.error("--window must be positive")
    timeout = args.timeout if args.timeout > 0 else max(10.0, args.bytes / 4000.0)

    results = {}
    if args.read_only and args.out_only:
        parser.error("--read-only and --out-only are mutually exclusive")

    workers = [
        threading.Thread(target=run_port, args=(path, args.bytes, 0x6D2B79F5 ^ index, args.baud, 0 if args.read_only else args.write_chunk, args.window, args.out_only, timeout, results))
        for index, path in enumerate(args.ports)
    ]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()

    for path in args.ports:
        print(f"{path}: {results[path]}")
    return 0 if all(result.startswith("PASS") for result in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
