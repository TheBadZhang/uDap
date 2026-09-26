#!/usr/bin/env python3
"""Exercise one CDC/UART pair with an ESP32-S3 esptool-like flash exchange."""

import argparse
import os
import queue
import select
import struct
import sys
import termios
import threading
import time
from collections import deque

from usb_cdc_loopback_test import configure, drain, payload


SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD
ESP_SYNC = 0x08
ESP_CHANGE_BAUDRATE = 0x0F
ESP_FLASH_DEFL_DATA = 0x11
ESP_CHECKSUM_MAGIC = 0xEF


def slip_encode(data: bytes) -> bytes:
    encoded = bytearray([SLIP_END])
    for value in data:
        if value == SLIP_END:
            encoded.extend((SLIP_ESC, SLIP_ESC_END))
        elif value == SLIP_ESC:
            encoded.extend((SLIP_ESC, SLIP_ESC_ESC))
        else:
            encoded.append(value)
    encoded.append(SLIP_END)
    return bytes(encoded)


class SlipReader:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.escaped = False
        self.frames: deque[bytes] = deque()

    def feed(self, data: bytes) -> None:
        for value in data:
            if value == SLIP_END:
                if self.buffer:
                    self.frames.append(bytes(self.buffer))
                    self.buffer.clear()
                self.escaped = False
            elif self.escaped:
                if value == SLIP_ESC_END:
                    self.buffer.append(SLIP_END)
                elif value == SLIP_ESC_ESC:
                    self.buffer.append(SLIP_ESC)
                else:
                    raise ValueError(f"invalid SLIP escape 0x{value:02x}")
                self.escaped = False
            elif value == SLIP_ESC:
                self.escaped = True
            else:
                self.buffer.append(value)


def write_all(fd: int, data: bytes, deadline: float) -> None:
    offset = 0
    while offset < len(data):
        if time.monotonic() > deadline:
            raise TimeoutError(f"write timeout at {offset}/{len(data)} bytes")
        _, writable, _ = select.select([], [fd], [], 0.1)
        if fd in writable:
            offset += os.write(fd, data[offset:offset + 4096])


def read_frame(
    fd: int,
    reader: SlipReader,
    deadline: float,
    peer_errors: queue.Queue[BaseException] | None = None,
) -> bytes:
    while not reader.frames:
        if peer_errors is not None and not peer_errors.empty():
            raise RuntimeError("UART-side emulator failed") from peer_errors.get()
        if time.monotonic() > deadline:
            raise TimeoutError("SLIP frame timeout")
        readable, _, _ = select.select([fd], [], [], 0.1)
        if fd in readable:
            data = os.read(fd, 4096)
            if data:
                reader.feed(data)
    return reader.frames.popleft()


def checksum(data: bytes) -> int:
    value = ESP_CHECKSUM_MAGIC
    for byte in data:
        value ^= byte
    return value


def request(opcode: int, body: bytes = b"", check: int = 0) -> bytes:
    return slip_encode(struct.pack("<BBHI", 0, opcode, len(body), check) + body)


def response(opcode: int) -> bytes:
    status = b"\x00\x00\x00\x00"
    return slip_encode(struct.pack("<BBHI", 1, opcode, len(status), 0) + status)


def parse_request(frame: bytes) -> tuple[int, bytes, int]:
    if len(frame) < 8:
        raise ValueError(f"short request: {len(frame)} bytes")
    direction, opcode, size, check = struct.unpack_from("<BBHI", frame)
    body = frame[8:]
    if direction != 0 or len(body) != size:
        raise ValueError(
            f"invalid request header direction={direction} size={size} actual={len(body)}"
        )
    return opcode, body, check


def expect_response(frame: bytes, opcode: int) -> None:
    if len(frame) != 12:
        raise ValueError(f"invalid response size: {len(frame)}")
    direction, actual_opcode, size, value = struct.unpack_from("<BBHI", frame)
    if direction != 1 or actual_opcode != opcode or size != 4 or value != 0:
        raise ValueError(
            f"invalid response direction={direction} opcode=0x{actual_opcode:02x} "
            f"size={size} value={value}"
        )
    if frame[8:] != b"\x00\x00\x00\x00":
        raise ValueError(f"ESP status failure: {frame[8:].hex()}")


def uart_emulator(
    uart_fd: int,
    expected: bytes,
    block_size: int,
    baud: int,
    timeout: float,
    flash_delay: float,
    switched: threading.Event,
    errors: queue.Queue[BaseException],
) -> None:
    reader = SlipReader()
    try:
        opcode, body, _ = parse_request(
            read_frame(uart_fd, reader, time.monotonic() + timeout)
        )
        if opcode != ESP_SYNC or body != b"\x07\x07\x12\x20" + b"\x55" * 32:
            raise ValueError("invalid esptool sync request")
        write_all(uart_fd, response(ESP_SYNC), time.monotonic() + timeout)

        opcode, body, _ = parse_request(
            read_frame(uart_fd, reader, time.monotonic() + timeout)
        )
        if opcode != ESP_CHANGE_BAUDRATE or body != struct.pack("<II", baud, 115200):
            raise ValueError("invalid esptool baud-rate change request")
        write_all(uart_fd, response(ESP_CHANGE_BAUDRATE), time.monotonic() + timeout)
        if not switched.wait(timeout):
            raise TimeoutError("baud switch timeout")

        received = 0
        sequence = 0
        while received < len(expected):
            opcode, body, actual_checksum = parse_request(
                read_frame(uart_fd, reader, time.monotonic() + timeout)
            )
            if opcode != ESP_FLASH_DEFL_DATA or len(body) < 16:
                raise ValueError(f"unexpected flash request opcode=0x{opcode:02x}")
            data_size, actual_sequence, zero0, zero1 = struct.unpack_from(
                "<IIII", body
            )
            block = body[16:]
            if data_size != len(block) or actual_sequence != sequence or zero0 or zero1:
                raise ValueError(
                    f"invalid block header sequence={actual_sequence} expected={sequence} "
                    f"data_size={data_size} actual={len(block)}"
                )
            expected_block = expected[received:received + block_size]
            if block != expected_block:
                mismatch = next(
                    index
                    for index, (actual, wanted) in enumerate(zip(block, expected_block))
                    if actual != wanted
                )
                raise ValueError(
                    f"block {sequence} mismatch at payload offset {received + mismatch}"
                )
            if actual_checksum != checksum(block):
                raise ValueError(f"block {sequence} checksum mismatch")
            if flash_delay:
                time.sleep(flash_delay)
            write_all(
                uart_fd,
                response(ESP_FLASH_DEFL_DATA),
                time.monotonic() + timeout,
            )
            received += len(block)
            sequence += 1
    except BaseException as exc:
        errors.put(exc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cdc", help="CH32 CDC tty used by the esptool-side sender")
    parser.add_argument("uart", help="external USB-UART tty emulating ESP32-S3")
    parser.add_argument("--baud", type=int, default=3000000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--block-size", type=int, default=0x4000)
    parser.add_argument("--flash-delay-ms", type=float, default=0.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    if args.bytes <= 0 or args.block_size <= 0 or args.bytes % args.block_size:
        parser.error("bytes must be a positive multiple of block-size")
    if args.flash_delay_ms < 0 or args.timeout <= 0:
        parser.error("flash-delay-ms must be non-negative and timeout must be positive")

    expected = payload(args.bytes, 0xE5325A17)
    cdc_fd = os.open(args.cdc, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    uart_fd = os.open(args.uart, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    errors: queue.Queue[BaseException] = queue.Queue()
    switched = threading.Event()
    server = threading.Thread(
        target=uart_emulator,
        args=(
            uart_fd,
            expected,
            args.block_size,
            args.baud,
            args.timeout,
            args.flash_delay_ms / 1000.0,
            switched,
            errors,
        ),
        daemon=True,
    )
    try:
        configure(cdc_fd, 115200)
        configure(uart_fd, 115200)
        termios.tcflush(cdc_fd, termios.TCIOFLUSH)
        termios.tcflush(uart_fd, termios.TCIOFLUSH)
        drain(cdc_fd)
        drain(uart_fd)
        server.start()

        host_reader = SlipReader()
        sync_body = b"\x07\x07\x12\x20" + b"\x55" * 32
        write_all(cdc_fd, request(ESP_SYNC, sync_body), time.monotonic() + args.timeout)
        expect_response(
            read_frame(cdc_fd, host_reader, time.monotonic() + args.timeout, errors),
            ESP_SYNC,
        )

        change_body = struct.pack("<II", args.baud, 115200)
        write_all(
            cdc_fd,
            request(ESP_CHANGE_BAUDRATE, change_body),
            time.monotonic() + args.timeout,
        )
        expect_response(
            read_frame(cdc_fd, host_reader, time.monotonic() + args.timeout, errors),
            ESP_CHANGE_BAUDRATE,
        )

        configure(cdc_fd, args.baud)
        configure(uart_fd, args.baud)
        time.sleep(0.05)
        switched.set()

        started = time.monotonic()
        for sequence, offset in enumerate(range(0, len(expected), args.block_size)):
            block = expected[offset:offset + args.block_size]
            body = struct.pack("<IIII", len(block), sequence, 0, 0) + block
            deadline = time.monotonic() + args.timeout
            write_all(
                cdc_fd,
                request(ESP_FLASH_DEFL_DATA, body, checksum(block)),
                deadline,
            )
            expect_response(
                read_frame(cdc_fd, host_reader, deadline, errors),
                ESP_FLASH_DEFL_DATA,
            )
        elapsed = time.monotonic() - started
        server.join(args.timeout)
        if server.is_alive():
            raise TimeoutError("UART-side emulator did not finish")
        if not errors.empty():
            raise RuntimeError("UART-side emulator failed") from errors.get()

        print(
            f"{args.cdc}: PASS {args.bytes} flash bytes in {elapsed:.3f}s "
            f"({args.bytes / elapsed / 1024:.1f} KiB/s), "
            f"{args.bytes // args.block_size} blocks via {args.uart}"
        )
        return 0
    finally:
        switched.set()
        os.close(cdc_fd)
        os.close(uart_fd)


if __name__ == "__main__":
    sys.exit(main())
