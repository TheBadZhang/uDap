#!/usr/bin/env python3
"""Strict two-port UART full-duplex test for the X035 CDC bridge."""

import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import threading
import time


IOSSIOSPEED = 0x80085402


def payload(size: int, seed: int) -> bytes:
    value = seed
    data = bytearray(size)
    for index in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[index] = value & 0xFF
    return bytes(data)


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
    deadline = time.monotonic() + 0.25
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.01)
        if not readable:
            continue
        if not os.read(fd, 4096):
            return


def run_stream(name: str, tx_fd: int, rx_fd: int, expected: bytes, write_chunk: int, window: int,
               timeout: float, min_rate: float, start: threading.Event, results: dict[str, str]) -> None:
    try:
        start.wait()
        sent = 0
        received = bytearray()
        started = time.monotonic()
        deadline = started + timeout
        while len(received) < len(expected):
            if time.monotonic() > deadline:
                raise TimeoutError(f"timeout sent={sent} received={len(received)}")
            can_write = sent < len(expected) and sent - len(received) < window
            readable, writable, _ = select.select([rx_fd], [tx_fd] if can_write else [], [], 0.1)
            if readable:
                data = os.read(rx_fd, min(4096, len(expected) - len(received)))
                if data:
                    begin = len(received)
                    received.extend(data)
                    if received[begin:] != expected[begin:len(received)]:
                        for offset, actual in enumerate(received[begin:]):
                            wanted = expected[begin + offset]
                            if actual != wanted:
                                raise ValueError(
                                    f"mismatch at {begin + offset}: got=0x{actual:02x} expected=0x{wanted:02x}")
            if writable:
                sent += os.write(tx_fd, expected[sent:sent + write_chunk])
        elapsed = time.monotonic() - started
        rate = len(expected) / elapsed / 1024
        if rate < min_rate:
            raise ValueError(f"rate {rate:.1f} KiB/s is below {min_rate:.1f} KiB/s")
        results[name] = f"PASS {len(expected)} bytes in {elapsed:.3f}s ({rate:.1f} KiB/s)"
    except Exception as exc:
        results[name] = f"FAIL {exc}"


def run_peer_tx_saturate(fd: int, start: threading.Event, stop: threading.Event, results: dict[str, str]) -> None:
    source = payload(1024, 0x3F52A9C1)

    try:
        start.wait()
        while not stop.is_set():
            _, writable, _ = select.select([], [fd], [], 0.1)
            if writable:
                os.write(fd, source)
    except Exception as exc:
        results["peer TX saturation"] = f"FAIL {exc}"
        stop.set()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("x035_port")
    parser.add_argument("peer_port")
    parser.add_argument("--baud", type=int, default=3_000_000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--write-chunk", type=int, default=1024)
    parser.add_argument("--window", type=int, default=8192)
    parser.add_argument("--timeout", type=float, default=0.0)
    parser.add_argument("--min-rate", type=float, default=0.0)
    parser.add_argument("--direction", choices=("both", "x035-to-peer", "peer-to-x035"), default="both")
    parser.add_argument("--peer-tx-saturate", action="store_true")
    args = parser.parse_args()
    if args.baud <= 0 or args.bytes <= 0 or args.write_chunk <= 0 or args.window <= 0 or args.min_rate < 0:
        parser.error("baud, bytes, write-chunk, and window must be positive; min-rate must not be negative")
    if args.peer_tx_saturate and args.direction != "x035-to-peer":
        parser.error("--peer-tx-saturate requires --direction x035-to-peer")

    timeout = args.timeout if args.timeout > 0 else max(20.0, args.bytes / 2000.0)
    x035_fd = os.open(args.x035_port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    peer_fd = os.open(args.peer_port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(x035_fd, args.baud)
        configure(peer_fd, args.baud)
        time.sleep(0.1)
        drain(x035_fd)
        drain(peer_fd)

        start = threading.Event()
        stop = threading.Event()
        results: dict[str, str] = {}
        streams = []
        if args.direction in ("both", "x035-to-peer"):
            streams.append(threading.Thread(target=run_stream, args=(
                "X035 TX -> peer RX", x035_fd, peer_fd, payload(args.bytes, 0x6D2B79F5), args.write_chunk,
                args.window, timeout, args.min_rate, start, results)))
        if args.direction in ("both", "peer-to-x035"):
            streams.append(threading.Thread(target=run_stream, args=(
                "peer TX -> X035 RX", peer_fd, x035_fd, payload(args.bytes, 0xA5C3F197), args.write_chunk,
                args.window, timeout, args.min_rate, start, results)))
        for stream in streams:
            stream.start()
        saturator = None
        if args.peer_tx_saturate:
            saturator = threading.Thread(target=run_peer_tx_saturate, args=(peer_fd, start, stop, results))
            saturator.start()
        start.set()
        for stream in streams:
            stream.join()
        stop.set()
        if saturator is not None:
            saturator.join()

        for name in results:
            print(f"{name}: {results[name]}")
        return 0 if all(result.startswith("PASS") for result in results.values()) else 1
    finally:
        os.close(x035_fd)
        os.close(peer_fd)


if __name__ == "__main__":
    sys.exit(main())
