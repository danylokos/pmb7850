"""Helpers for synthetic Siemens full-flash test images."""

from pathlib import Path

def seed_metadata(
    image: bytearray,
    *,
    model: str = "C55",
    software: int = 0x24,
    langpack: str = "lg1",
    bcore_software: int = 0x24,
    manufacturer: int = 0x0020,
    device: int = 0x0017,
    view: int = 0,
) -> None:
    metadata = view + 0x7FF50
    image[metadata:metadata + 16] = bytes(
        (software, 0xFF, 0x0A, 0x50, 0x14, 0x14, 0x01, 0x00,
         0xD5, 0x21, 0x20, 0x00, 0xFF, 0xFF, 0xFF, 0xFF)
    )
    image[metadata + 0x10:metadata + 0x40] = bytes(0x30)
    image[metadata + 0x10:metadata + 0x10 + len(langpack)] = langpack.encode()
    image[metadata + 0x20:metadata + 0x20 + len(model)] = model.encode()
    image[metadata + 0x30:metadata + 0x38] = b"SIEMENS\0"
    image[view + 0x32C] = bcore_software
    image[view + 0x7FE26:view + 0x7FE2A] = (
        manufacturer.to_bytes(2, "little") + device.to_bytes(2, "little")
    )
def seed_metadata_file(path: Path, **kwargs: object) -> None:
    image = bytearray(path.read_bytes())
    seed_metadata(image, **kwargs)
    path.write_bytes(image)
