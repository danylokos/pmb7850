from __future__ import annotations

from .codecs import (
    COMPRESSED_BITMAP_TYPE,
    DESCRIPTOR_SIZE,
    RAW_BITMAP_TYPE,
    BitmapDescriptor,
    DescriptorRun,
    ScannedDescriptor,
    decode_bitmap,
    resolve_encoding,
)

def scan_image_descriptors(
    image: bytes,
    flash_base: int,
    max_width: int = 255,
    max_height: int = 255,
    min_pixels: int = 1,
    kinds: tuple[int, ...] = (RAW_BITMAP_TYPE, COMPRESSED_BITMAP_TYPE),
    start_address: int | None = None,
    end_address: int | None = None,
    encoding: str = "auto",
) -> list[ScannedDescriptor]:
    """Find independently valid bitmap descriptors at every byte offset."""
    for kind in kinds:
        resolve_encoding(kind, encoding)
    candidates: list[ScannedDescriptor] = []
    first_address = flash_base if start_address is None else start_address
    last_address = flash_base + len(image) - 1 if end_address is None else end_address
    for kind in kinds:
        position = max(0, first_address - flash_base + 2)
        marker_bytes = bytes((kind, 0))
        while True:
            marker = image.find(marker_bytes, position)
            if marker < 0:
                break
            position = marker + 1
            offset = marker - 2
            address = flash_base + offset
            if address > last_address:
                break
            if offset < 0 or offset + DESCRIPTOR_SIZE > len(image):
                continue
            descriptor = BitmapDescriptor.from_bytes(
                0, address, image[offset:offset + DESCRIPTOR_SIZE]
            )
            if (
                not descriptor.is_structurally_valid(flash_base, len(image))
                or descriptor.kind != kind
                or descriptor.width > max_width
                or descriptor.height > max_height
                or descriptor.width * descriptor.height < min_pixels
            ):
                continue
            source_offset = descriptor.source_address - flash_base
            try:
                _, encoded_bytes = decode_bitmap(
                    image[source_offset:], descriptor, encoding
                )
            except ValueError:
                continue
            candidates.append(
                ScannedDescriptor(
                    descriptor=descriptor,
                    encoded_bytes=encoded_bytes,
                    encoding=resolve_encoding(descriptor.kind, encoding),
                )
            )
    candidates.sort(key=lambda item: item.descriptor.address)
    return candidates


def find_descriptor_runs(
    candidates: list[ScannedDescriptor],
    strides: tuple[int, ...],
    min_run: int = 2,
) -> list[DescriptorRun]:
    """Group candidates into fixed-stride runs, suppressing wider subsamples."""
    if min_run < 2:
        raise ValueError("minimum descriptor run must be at least two")
    if any(stride < DESCRIPTOR_SIZE for stride in strides):
        raise ValueError("scan strides must be at least eight bytes")

    by_address = {item.descriptor.address: item for item in candidates}
    claimed: set[int] = set()
    runs: list[DescriptorRun] = []
    for stride in sorted(set(strides)):
        for address in sorted(by_address):
            if address - stride in by_address:
                continue
            entries: list[ScannedDescriptor] = []
            cursor = address
            while cursor in by_address:
                entries.append(by_address[cursor])
                cursor += stride
            entry_addresses = {item.descriptor.address for item in entries}
            if len(entries) < min_run or entry_addresses <= claimed:
                continue
            runs.append(DescriptorRun(stride=stride, entries=tuple(entries)))
            claimed.update(entry_addresses)
    return runs


def scan_entry_dict(item: ScannedDescriptor) -> dict[str, int | str | bool]:
    descriptor = item.descriptor
    source_end = descriptor.source_address + item.encoded_bytes
    return {
        "descriptor_address": f"0x{descriptor.address:06x}",
        "source_address": f"0x{descriptor.source_address:06x}",
        "source_end": f"0x{source_end:06x}",
        "width": descriptor.width,
        "height": descriptor.height,
        "kind": f"0x{descriptor.kind:02x}",
        "encoding": item.encoding,
        "encoded_bytes": item.encoded_bytes,
        "source_to_descriptor_gap": descriptor.address - source_end,
        "source_ends_at_descriptor": source_end == descriptor.address,
    }
