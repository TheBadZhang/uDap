#!/usr/bin/env python3
"""Strict simultaneous full-duplex test for one CDC and one external UART."""

import argparse
import os
import select
import sys
import termios
import threading
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


def run_pair(cdc: str, uart: str, args: argparse.Namespace, results: dict[str, str]) -> None:
    cdc_fd = None
    uart_fd = None
    try:
        cdc_fd = os.open(cdc, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        uart_fd = os.open(uart, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure(cdc_fd, args.baud)
        configure(uart_fd, args.baud)
        time.sleep(0.1)
        termios.tcflush(cdc_fd, termios.TCIOFLUSH)
        termios.tcflush(uart_fd, termios.TCIOFLUSH)
        drain(cdc_fd)
        drain(uart_fd)

        cdc_to_uart = payload(args.bytes, 0x6D2B79F5)
        uart_to_cdc = payload(args.bytes, 0x6D2B79F5 ^ 1)
        sent_cdc = sent_uart = 0
        received_uart = bytearray()
        received_cdc = bytearray()
        deadline = time.monotonic() + args.timeout
        started = time.monotonic()

        while len(received_uart) < args.bytes or len(received_cdc) < args.bytes:
            if time.monotonic() > deadline:
                raise TimeoutError(
                    f"timeout CDC->UART sent={sent_cdc} received={len(received_uart)}; "
                    f"UART->CDC sent={sent_uart} received={len(received_cdc)}"
                )

            cdc_can_write = sent_cdc < args.bytes and sent_cdc - len(received_uart) < args.window
            uart_can_write = sent_uart < args.bytes and sent_uart - len(received_cdc) < args.window
            readable, writable, _ = select.select(
                [cdc_fd, uart_fd],
                ([cdc_fd] if cdc_can_write else []) + ([uart_fd] if uart_can_write else []),
                [],
                0.1,
            )

            if uart_fd in readable:
                data = os.read(uart_fd, min(4096, args.bytes - len(received_uart)))
                if data:
                    begin = len(received_uart)
                    received_uart.extend(data)
                    actual = received_uart[begin:]
                    expected = cdc_to_uart[begin:len(received_uart)]
                    if actual != expected:
                        offset = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                        raise ValueError(
                            f"CDC->UART mismatch at {begin + offset}: "
                            f"got={actual[offset:offset + 8].hex()} expected={expected[offset:offset + 8].hex()}"
                        )

            if cdc_fd in readable:
                data = os.read(cdc_fd, min(4096, args.bytes - len(received_cdc)))
                if data:
                    begin = len(received_cdc)
                    received_cdc.extend(data)
                    actual = received_cdc[begin:]
                    expected = uart_to_cdc[begin:len(received_cdc)]
                    if actual != expected:
                        offset = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                        raise ValueError(
                            f"UART->CDC mismatch at {begin + offset}: "
                            f"got={actual[offset:offset + 8].hex()} expected={expected[offset:offset + 8].hex()}"
                        )

            if cdc_fd in writable:
                sent_cdc += os.write(cdc_fd, cdc_to_uart[sent_cdc:sent_cdc + args.write_chunk])
            if uart_fd in writable:
                sent_uart += os.write(uart_fd, uart_to_cdc[sent_uart:sent_uart + args.write_chunk])

        elapsed = time.monotonic() - started
        rate = args.bytes / elapsed / 1024.0
        results[cdc] = (
            f"PASS {args.bytes} bytes/direction in {elapsed:.3f}s "
            f"({rate:.1f} KiB/s/direction) via {uart}"
        )
    except Exception as exc:
        results[cdc] = f"FAIL via {uart}: {exc}"
    finally:
        if cdc_fd is not None:
            os.close(cdc_fd)
        if uart_fd is not None:
            os.close(uart_fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="+", help="CDC and external UART tty pairs: cdc0 uart0 [cdc1 uart1]")
    parser.add_argument("--baud", type=int, default=3000000)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--write-chunk", type=int, default=251)
    parser.add_argument("--window", type=int, default=1024)
    parser.add_argument("--timeout", type=float, default=45.0)
    args = parser.parse_args()

    if len(args.ports) == 0 or len(args.ports) > 4 or len(args.ports) % 2 != 0:
        parser.error("supply one or two CDC/UART tty pairs")
    if args.bytes <= 0 or args.write_chunk <= 0 or args.window <= 0:
        parser.error("bytes, write-chunk, and window must be positive")

    results: dict[str, str] = {}
    workers = [
        threading.Thread(target=run_pair, args=(args.ports[index], args.ports[index + 1], args, results))
        for index in range(0, len(args.ports), 2)
    ]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()

    for cdc in args.ports[::2]:
        print(f"{cdc}: {results[cdc]}")
    return 0 if all(result.startswith("PASS") for result in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
