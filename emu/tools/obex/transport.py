"""Asynchronous, exclusive pySerial byte transport."""

from __future__ import annotations

import asyncio
import errno
import json
import time
from pathlib import Path

from .errors import PortBusyError, TransportError
from .models import ClientConfig


class SerialTransport:
    def __init__(self, config: ClientConfig) -> None:
        self.config = config
        self._serial = None
        self._trace = None

    async def open(self) -> None:
        if self._trace is None and self.config.wire_trace:
            self._trace = Path(self.config.wire_trace).open("a", encoding="utf-8")
        try:
            import serial
            kwargs = dict(
                port=self.config.port, baudrate=self.config.baud,
                timeout=min(self.config.timeout, 0.25), write_timeout=min(self.config.timeout, 2.0),
                bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE, xonxoff=False, rtscts=False, dsrdtr=False,
            )
            if __import__("os").name == "posix":
                kwargs["exclusive"] = True
            # pySerial calls are intentionally kept on this serialized client's
            # event-loop thread. Python 3.14 can stall or crash when the POSIX
            # backend is entered through ThreadPoolExecutor. Poll availability
            # asynchronously so stalled/fragmented reads remain cancellable.
            self._serial = serial.Serial(**kwargs)
            self._serial.reset_input_buffer()
        except Exception as exc:
            if getattr(exc, "errno", None) in (errno.EACCES, errno.EBUSY, errno.EAGAIN):
                raise PortBusyError(f"serial port is busy: {self.config.port}") from exc
            raise TransportError(f"cannot open serial port {self.config.port}: {exc}") from exc

    def trace(self, direction: str, phase: str, data: bytes) -> None:
        if self._trace is None:
            return
        self._trace.write(json.dumps({
            "time": time.time(), "direction": direction, "phase": phase,
            "length": len(data), "data": data.hex(),
        }, separators=(",", ":")) + "\n")
        self._trace.flush()

    async def write_all(self, data: bytes, phase: str) -> None:
        if self._serial is None:
            raise TransportError("serial transport is not open")
        self.trace("tx", phase, data)
        try:
            written = self._serial.write(data)
            if written != len(data):
                raise TransportError(f"short serial write: {written}/{len(data)}")
            self._serial.flush()
        except TransportError:
            raise
        except Exception as exc:
            raise TransportError(f"serial write failed: {exc}") from exc

    async def read_exactly(self, length: int, phase: str, timeout: float | None = None) -> bytes:
        if self._serial is None:
            raise TransportError("serial transport is not open")
        deadline = time.monotonic() + (self.config.timeout if timeout is None else timeout)
        result = bytearray()
        while len(result) < length:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out reading {phase}")
            try:
                available = self._serial.in_waiting
                if not available:
                    await asyncio.sleep(0.001)
                    continue
                chunk = self._serial.read(min(available, length - len(result)))
            except Exception as exc:
                raise TransportError(f"serial read failed: {exc}") from exc
            if chunk:
                result.extend(chunk)
        data = bytes(result)
        self.trace("rx", phase, data)
        return data

    async def close(self) -> None:
        serial_port, self._serial = self._serial, None
        if serial_port is not None:
            try:
                serial_port.close()
            except Exception:
                pass
        if self._trace is not None:
            self._trace.close()
            self._trace = None
