"""Linux loopback benchmark. USB behavior is supplied by the mock backend."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import os
from pathlib import Path
import statistics
import tempfile
import threading
import time

from integration import connect, free_ports, header, payload, query, receive, send, server


MIB = 1024 * 1024


def download(connection, size, block):
    send(connection, f"download:{size:08x}".encode())
    assert receive(connection) == f"DATA{size:08x}".encode()
    remaining = size
    while remaining:
        frame = min(512 * MIB, remaining)
        connection.sendall(header(frame))
        frame_left = frame
        while frame_left:
            part = min(len(block), frame_left)
            connection.sendall(memoryview(block)[:part])
            frame_left -= part
        remaining -= frame
    assert receive(connection) == b"OKAY"
    query(connection, b"mock-download-size", f"{size:08x}".encode())


def timed(port, sizes, block, barrier=None):
    with connect(port) as connection:
        if barrier:
            barrier.wait()
        started = time.perf_counter()
        for size in sizes:
            download(connection, size, block)
        return time.perf_counter() - started


def process_stats(pid):
    entries = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    cpu = (int(entries[11]) + int(entries[12])) / os.sysconf("SC_CLK_TCK")
    status = dict(line.rstrip().split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines())
    return cpu, status["VmRSS"].strip(), status["VmHWM"].strip()


def measure(exe, work):
    a, b = free_ports(2)
    config = {
        "usb_backend": "mock", "log_level": "WARN",
        "devices": [{"serial": "MOCK_A", "port": a}, {"serial": "MOCK_B", "port": b}],
    }
    block = payload(MIB)
    with server(exe, work, config) as instance:
        with connect(a) as connection:
            latency = []
            for _ in range(200):
                started = time.perf_counter_ns()
                query(connection, b"product", b"proxy-mock")
                latency.append((time.perf_counter_ns() - started) / 1000)
            print(f"getvar: median {statistics.median(latency):.1f} µs, p95 {statistics.quantiles(latency, n=20)[18]:.1f} µs")
        baseline = process_stats(instance.pid)
        print(f"baseline RSS {baseline[1]}")
        for amounts in ([MIB], [100 * MIB], [1024 * MIB], [2048 * MIB, 2048 * MIB]):
            start_cpu, _, _ = process_stats(instance.pid)
            duration = timed(a, amounts, block)
            cpu, rss, peak = process_stats(instance.pid)
            total = sum(amounts) / MIB
            print(f"{total:.0f} MiB: {duration:.3f} s, {total / duration:.1f} MiB/s, "
                  f"server CPU {(cpu - start_cpu) * 100 / duration:.1f}% of one logical core, RSS {rss}, peak {peak}")
        with ThreadPoolExecutor(max_workers=2) as pool:
            barrier = threading.Barrier(2)
            jobs = [pool.submit(timed, port, [100 * MIB], block, barrier) for port in (a, b)]
            elapsed = max(job.result() for job in jobs)
            print(f"2 × 100 MiB: {elapsed:.3f} s, aggregate {200 / elapsed:.1f} MiB/s")
        with contextlib.closing(connect(a)) as connection:
            send(connection, b"reboot-fastboot")
            assert receive(connection) == b"OKAYrebooting"
            started = time.perf_counter()
        with connect(a) as connection:
            query(connection, b"is-userspace", b"yes")
        print(f"simulated reattach: {(time.perf_counter() - started) * 1000:.1f} ms")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("proxy", type=Path)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fastboot-proxy-bench-") as directory:
        measure(str(options.proxy.resolve()), Path(directory))
