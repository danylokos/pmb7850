# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronous QMP client for bounded qualification sessions."""
import json
import socket


class QMP:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.file = sock.makefile("rwb", buffering=0)
        greeting = self._receive()
        if "QMP" not in greeting:
            raise RuntimeError("QMP greeting is missing")
        self.execute("qmp_capabilities")

    def _receive(self) -> dict:
        while True:
            line = self.file.readline()
            if not line:
                raise EOFError("QMP socket closed")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command: str, arguments: dict | None = None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode() + b"\n")
        response = self._receive()
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']}")
        return response.get("return")

    def close(self) -> None:
        self.file.close()
