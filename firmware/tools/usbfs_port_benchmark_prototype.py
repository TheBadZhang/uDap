#!/usr/bin/env python3
"""PROTOTYPE: measure the CH32X035 USBFS DCD without UART wiring.

Question: what sustained, byte-exact throughput does the current CherryUSB DCD
and dual-CDC data path deliver for device-to-host, host-to-device, and echo
traffic, both per port and with both ports active?

The script temporarily builds and flashes one firmware mode at a time. It
restores the previous xmake configuration and normal firmware unless
--keep-benchmark-firmware is supplied.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from usb_cdc_loopback_test import run_port


MODE_FLAGS = {
    "source": "-DUSBFS_PORT_BENCHMARK=1 -DUSB_SERIAL_PATTERN_TEST=1 -DUSB_SERIAL_OUT_TEST=0",
    "sink": "-DUSBFS_PORT_BENCHMARK=1 -DUSB_SERIAL_PATTERN_TEST=1 -DUSB_SERIAL_OUT_TEST=1",
    "echo": "-DUSBFS_PORT_BENCHMARK=1 -DUSB_SERIAL_PATTERN_TEST=0 -DUSB_SERIAL_ECHO_TEST=1",
}


def run(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def configure_and_build(repo: Path, xmake: str, mode: str, count: int) -> Path:
    flags = f"{MODE_FLAGS[mode]} -DUSB_SERIAL_TEST_BYTES={count}u"
    run([xmake, "f", "-c", "--mode=release", f"--cxflags={flags}"], repo)
    run([xmake, "-r"], repo)
    return repo / "build/release/firmware.elf"


def restore_build(repo: Path, xmake: str, config: Path) -> Path:
    run([xmake, "f", f"--import={config}"], repo)
    # The exported global config does not carry target-local cxflags. Clear the
    # temporary benchmark flags explicitly before rebuilding the application.
    run([xmake, "f", "-c", "--mode=release", "--cxflags="], repo)
    run([xmake, "-r"], repo)
    return repo / "build/release/firmware.elf"


def flash(repo: Path, wlink: str, image: Path) -> None:
    run([wlink, "--chip", "CH32X035", "flash", "-e", str(image)], repo)


def wait_for_ports(paths: list[str], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    disconnected = not all(os.path.exists(path) for path in paths)
    while time.monotonic() < deadline:
        present = all(os.path.exists(path) for path in paths)
        if not present:
            disconnected = True
        if disconnected and present:
            time.sleep(1.0)
            return
        time.sleep(0.1)
    missing = [path for path in paths if not os.path.exists(path)]
    raise TimeoutError(f"CDC ports did not enumerate: {', '.join(missing)}")


def measure(mode: str, ports: list[tuple[str, int]], count: int, write_chunk: int,
            window: int, timeout: float) -> dict[str, str]:
    results: dict[str, str] = {}
    workers = []
    for path, port_index in ports:
        workers.append(threading.Thread(
            target=run_port,
            args=(path, count, 0x6D2B79F5 ^ port_index, 115200,
                  0 if mode == "source" else write_chunk, window,
                  mode == "sink", timeout, results),
        ))
    started = time.monotonic()
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    wall_time = time.monotonic() - started
    results["__wall_time__"] = f"{wall_time:.3f}"
    return results


def print_results(mode: str, label: str, ports: list[tuple[str, int]], results: dict[str, str]) -> bool:
    print(f"\n[{mode} / {label}] wall={results['__wall_time__']}s")
    passed = True
    for path, _ in ports:
        result = results[path]
        print(f"  {path}: {result}")
        passed &= result.startswith("PASS")
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ports", nargs="+", help="CDC tty paths in firmware port order")
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--modes", default="source,sink,echo")
    parser.add_argument("--write-chunk", type=int, default=4096)
    parser.add_argument("--window", type=int, default=256 * 1024)
    parser.add_argument("--timeout", type=float, default=0.0)
    parser.add_argument("--port-wait", type=float, default=10.0)
    parser.add_argument("--xmake", default="xmake")
    parser.add_argument("--wlink", default=str(Path.home() / ".cargo/bin/wlink"))
    parser.add_argument("--keep-benchmark-firmware", action="store_true")
    args = parser.parse_args()

    if args.bytes <= 0 or args.write_chunk <= 0 or args.window <= 0:
        parser.error("--bytes, --write-chunk, and --window must be positive")
    modes = [mode.strip() for mode in args.modes.split(",") if mode.strip()]
    unknown = [mode for mode in modes if mode not in MODE_FLAGS]
    if unknown or not modes:
        parser.error(f"invalid modes: {', '.join(unknown) if unknown else args.modes}")

    repo = Path(__file__).resolve().parents[1]
    indexed_ports = list(zip(args.ports, range(len(args.ports))))
    scenarios = [(f"single-{index}", [port]) for index, port in enumerate(indexed_ports)]
    if len(indexed_ports) > 1:
        scenarios.append(("dual", indexed_ports))
    timeout = args.timeout if args.timeout > 0 else max(20.0, args.bytes / 10000.0)
    passed = True

    with tempfile.TemporaryDirectory(prefix="x035-usbfs-benchmark-") as temp_dir:
        config = Path(temp_dir) / "xmake-config.txt"
        run([args.xmake, "f", f"--export={config}"], repo)
        try:
            for mode in modes:
                print(f"\n=== Building {mode} benchmark firmware ===", flush=True)
                image = configure_and_build(repo, args.xmake, mode, args.bytes)
                flash(repo, args.wlink, image)
                wait_for_ports(args.ports, args.port_wait)
                for label, ports in scenarios:
                    results = measure(mode, ports, args.bytes, args.write_chunk,
                                      args.window, timeout)
                    passed &= print_results(mode, label, ports, results)
        finally:
            if args.keep_benchmark_firmware:
                run([args.xmake, "f", f"--import={config}"], repo)
                run([args.xmake, "f", "-c", "--mode=release", "--cxflags="], repo)
            else:
                print("\n=== Restoring normal firmware ===", flush=True)
                image = restore_build(repo, args.xmake, config)
                flash(repo, args.wlink, image)
                wait_for_ports(args.ports, args.port_wait)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
