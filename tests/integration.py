"""Exercise the production TCP/session path against the simulated USB backend."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import zipfile


PATTERN = b"OKAYFAILINFODATA00000000TEXT\x00\xff"
UPLOAD_SIZE = 5 * 1024 * 1024 + 127


def payload(size, offset=0):
    first = offset % len(PATTERN)
    return (PATTERN * ((first + size) // len(PATTERN) + 1))[first:first + size]


def fnv64(buffer):
    value = 14695981039346656037
    for ch in buffer:
        value = ((value ^ ch) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}".encode()


def header(size):
    return struct.pack("!Q", size)


def exact(connection, size):
    data = bytearray()
    while len(data) < size:
        block = connection.recv(min(size - len(data), 65536))
        if not block:
            raise EOFError("TCP EOF inside message")
        data.extend(block)
    return bytes(data)


def send(connection, message):
    connection.sendall(header(len(message)) + message)


def receive(connection):
    (size,) = struct.unpack("!Q", exact(connection, 8))
    assert 0 < size <= UPLOAD_SIZE, size
    return exact(connection, size)


def dropped(connection):
    try:
        assert connection.recv(1) == b""
    except (ConnectionResetError, ConnectionAbortedError):
        pass


def raw_connection(port):
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    connection.settimeout(5)
    return connection


def connect(port, version=b"FB01", retry=True):
    deadline = time.monotonic() + (8 if retry else 0)
    while True:
        connection = None
        try:
            connection = raw_connection(port)
            connection.sendall(version)
            assert exact(connection, 4) == b"FB01"
            return connection
        except (EOFError, OSError):
            if connection:
                connection.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(.045)


def query(connection, name, expected):
    send(connection, b"getvar:" + name)
    assert receive(connection) == b"OKAY" + expected


def free_ports(count):
    with contextlib.ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(count)]
        for candidate in sockets:
            candidate.bind(("127.0.0.1", 0))
        return [candidate.getsockname()[1] for candidate in sockets]


@contextlib.contextmanager
def server(exe, work, config, name="server"):
    path = work / f"{name}.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    with (work / f"{name}.log").open("w+", encoding="utf-8") as log:
        instance = subprocess.Popen([exe, "serve", "--config", str(path)], stdout=log, stderr=log)
        try:
            yield instance
        except BaseException:
            log.seek(0)
            print(log.read())
            raise
        finally:
            instance.terminate()
            try:
                instance.wait(timeout=5)
            except subprocess.TimeoutExpired:
                instance.kill()
                instance.wait()


def blocked(port):
    deadline = time.monotonic() + 4
    while True:
        try:
            with raw_connection(port) as connection:
                try:
                    connection.sendall(b"FB01")
                    dropped(connection)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass
                return
        except ConnectionRefusedError:
            if time.monotonic() > deadline:
                raise
            time.sleep(.04)


def raw_tests(exe, work):
    a, b, same, duplicate, located, no_serial = free_ports(6)
    config = {
        "usb_backend": "mock", "log_level": "DEBUG",
        "session_timeout_ms": 650, "usb_timeout_ms": 320, "io_timeout_ms": 800,
        "reconnect_timeout_ms": 1800,
        "devices": [
            {"serial": "MOCK_A", "port": a}, {"serial": "MOCK_B", "port": b},
            {"serial": "MOCK_A", "location": "mock-a", "port": same},
            {"serial": "DUPLICATE", "port": duplicate},
            {"serial": "DUPLICATE", "location": "mock-duplicate-2", "port": located},
            {"instance": "mock-no-serial-bootloader", "port": no_serial},
        ],
    }
    with server(exe, work, config):
        with connect(a, b"FB99") as connection:
            # The byte-at-a-time TCP stream contains two complete command frames.
            stream = header(14) + b"getvar:product" + header(10) + b"getvar:all"
            for ch in stream:
                connection.sendall(bytes([ch]))
            assert receive(connection) == b"OKAYproxy-mock"
            assert receive(connection) == b"INFOproduct: proxy-mock"
            assert receive(connection) == b"INFOserialno: MOCK_A"
            assert receive(connection) == b"TEXTmock-text\x00"
            assert receive(connection) == b"OKAY"
            blocked(a)
            blocked(same)
            with connect(b) as other:
                query(other, b"serialno", b"MOCK_B")
            send(connection, b"getvar:no-such-variable")
            assert receive(connection).startswith(b"FAIL")
            query(connection, b"product", b"proxy-mock")

        blocked(duplicate)
        with connect(located) as connection:
            query(connection, b"serialno", b"DUPLICATE")

        for hello in (b"FB00", b"NO01", b"FBa1"):
            with raw_connection(a) as connection:
                connection.sendall(hello)
                dropped(connection)
            time.sleep(.04)
        for size in (0, 4097, 0xffffffffffffffff):
            with connect(a) as connection:
                connection.sendall(header(size))
                dropped(connection)

        with connect(a) as connection:
            data = payload(1024 * 1024 + 931)
            send(connection, f"download:{len(data):08x}".encode())
            assert receive(connection) == f"DATA{len(data):08x}".encode()
            for offset in range(0, len(data), 32749):
                send(connection, data[offset:offset + 32749])
            assert receive(connection) == b"OKAY"
            query(connection, b"mock-download-hash", fnv64(data))
            send(connection, b"upload")
            assert receive(connection) == f"DATA{UPLOAD_SIZE:08x}".encode()
            assert hashlib.sha256(receive(connection)).digest() == hashlib.sha256(payload(UPLOAD_SIZE)).digest()
            assert receive(connection) == b"OKAY"

        with connect(a) as connection:
            send(connection, b"reboot-fastboot")
            assert receive(connection) == b"OKAYrebooting"
            dropped(connection)
        with connect(a) as connection:
            query(connection, b"is-userspace", b"yes")
            send(connection, b"reboot-bootloader")
            assert receive(connection) == b"OKAYrebooting"
            dropped(connection)
        with connect(a) as connection:
            query(connection, b"is-userspace", b"no")
            send(connection, b"oem mock-detach")
            dropped(connection)
        with connect(a) as connection:
            query(connection, b"product", b"proxy-mock")
            dropped(connection)  # Idle timeout releases an otherwise clean session.
        with connect(a) as connection:
            query(connection, b"product", b"proxy-mock")

        with connect(no_serial) as connection:
            send(connection, b"reboot-bootloader")
            assert receive(connection) == b"OKAYrebooting"
            dropped(connection)
        time.sleep(.7)
        blocked(no_serial)

        with connect(a) as connection:
            send(connection, b"download:00100000")
            assert receive(connection) == b"DATA00100000"
            send(connection, b"x" * 65536)
        time.sleep(.18)
        blocked(a)
        blocked(same)

        with connect(b) as connection:
            send(connection, b"oem mock-malformed")
            dropped(connection)
        blocked(b)
        with connect(located) as connection:
            send(connection, b"oem mock-timeout")
            dropped(connection)
        blocked(located)

    with server(exe, work, config, "restarted"):
        with connect(a) as connection:
            query(connection, b"product", b"proxy-mock")
            send(connection, b"download:00000010")
            assert receive(connection) == b"DATA00000010"
            connection.sendall(header(17))
            dropped(connection)
        blocked(a)
        with connect(b) as connection:
            send(connection, b"oem mock-unknown-data")
            dropped(connection)
        blocked(b)

    config["allowlist"] = ["192.0.2.0/24"]
    config["devices"] = [{"serial": "MOCK_A", "port": free_ports(1)[0]}]
    with server(exe, work, config, "allowlist"):
        blocked(config["devices"][0]["port"])


def official_tests(exe, fastboot, work):
    port = free_ports(1)[0]
    config = {"usb_backend": "mock", "log_level": "DEBUG",
              "devices": [{"serial": "MOCK_A", "port": port}]}
    target = f"tcp:127.0.0.1:{port}"
    env = dict(os.environ, HOME=str(work), USERPROFILE=str(work),
               ANDROID_USER_HOME=str(work), ANDROID_PRODUCT_OUT=str(work))

    def invoke(*args, success=True):
        command = [fastboot, "-s", target, *map(str, args)]
        result = subprocess.run(command, cwd=work, env=env, capture_output=True, timeout=20)
        message = result.stdout + result.stderr
        if success:
            assert result.returncode == 0, (command, result.returncode, message)
        return message

    with server(exe, work, config):
        assert b"proxy-mock" in invoke("getvar", "product")
        message = invoke("getvar", "all")
        assert b"serialno: MOCK_A" in message and b"mock-text" in message
        assert b"FAILED (remote: 'unsupported mock command')" in invoke(
            "getvar", "missing", success=False)

        original = work / "download.img"
        binary = payload(1300123)
        original.write_bytes(binary)
        invoke("stage", original)
        assert fnv64(binary) in invoke("getvar", "mock-download-hash")

        for command, arguments in (("get_staged", []), ("fetch", ["mock"])):
            output = work / f"{command}.img"
            invoke(command, *arguments, output)
            with output.open("rb") as stream:
                assert hashlib.file_digest(stream, "sha256").digest() == hashlib.sha256(
                    payload(UPLOAD_SIZE)).digest(), command

        invoke("reboot", "fastboot")
        assert b"is-userspace: yes" in invoke("getvar", "is-userspace")
        invoke("reboot", "bootloader")
        assert b"is-userspace: no" in invoke("getvar", "is-userspace")

        # Exercise AOSP reopening this same TCP address from inside FlashAllTool.
        # All target partitions and contents belong exclusively to the mock backend.
        sparse_image = (struct.pack("<I4H4I", 0xED26FF3A, 1, 0, 28, 12, 4096, 1, 1, 0)
                        + struct.pack("<2H2I", 0xCAC1, 0, 1, 4108) + payload(4096))
        fixtures = {"android-info.txt": b"require product=proxy-mock\n",
                    "fastboot-info.txt": b"version 1\nreboot fastboot\nflash mock mock.img\nreboot bootloader\n",
                    "mock.img": sparse_image}
        archive = work / "update.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            for name, data in fixtures.items():
                bundle.writestr(name, data)
                (work / name).write_bytes(data)
        for operation in (("update", archive), ("flashall",)):
            result = invoke("--skip-reboot", *operation)
            assert b"Writing 'mock'" in result and b"FAIL" not in result, result
            assert b"is-userspace: no" in invoke("getvar", "is-userspace")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("proxy")
    parser.add_argument("--fastboot")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fastboot-proxy-") as directory:
        work = Path(directory)
        if args.fastboot:
            official_tests(str(Path(args.proxy).resolve()), str(Path(args.fastboot).resolve()), work)
        else:
            raw_tests(str(Path(args.proxy).resolve()), work)
