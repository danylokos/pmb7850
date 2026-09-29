"""Typed failures shared by CLI and WebUI adapters."""


class ObexError(Exception):
    """Base error for the client library."""


class TransportError(ObexError):
    """The serial byte stream failed."""


class PortBusyError(TransportError):
    """Another process owns the selected serial port."""


class ProtocolError(ObexError):
    """The peer returned malformed or unexpected protocol data."""


class RemoteError(ObexError):
    """The peer returned an OBEX error response."""

    def __init__(self, response: int, message: str | None = None) -> None:
        self.response = response
        super().__init__(message or f"remote OBEX response 0x{response:02x}")


class UnsupportedFilesystemRootError(ValueError):
    """A drive designator has no proven mapping in the stock OBEX target."""

    def __init__(self, root: str) -> None:
        self.root = root
        super().__init__(
            f"unsupported Siemens OBEX filesystem root {root!r}: "
            "the stock target opens at 'A:' and no host-side drive selector is proven"
        )


class SessionUnavailable(TransportError):
    """An explicit connection or reconnect is required."""


class PathConflict(ObexError):
    def __init__(self, path):
        self.path = path
        super().__init__(f"target is a folder: {path}")


class FileNotFound(ObexError):
    pass


class OverwriteRequired(ObexError):
    def __init__(self, path, token):
        self.path, self.token = path, token
        super().__init__(f"overwrite confirmation required for {path}; use put --overwrite")
