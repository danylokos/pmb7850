"""Siemens bitmap descriptor decoding, extraction, and scanning."""

from .codecs import (
    ARGB4444_BITMAP_TYPE,
    AUTO_ENCODING_BY_TYPE,
    INDEXED2_BITMAP_TYPE,
    INDEXED4_BITMAP_TYPE,
    RGB332_BITMAP_TYPE,
    SUPPORTED_ENCODINGS,
    BitmapDescriptor,
    DescriptorRun,
    ExtractedBitmap,
    FramePlacement,
    ScannedDescriptor,
    compose_bitmap_stages,
    decode_argb4444,
    decode_bitmap,
    decode_compressed,
    decode_indexed2,
    decode_indexed4,
    decode_raw,
    decode_rgb332,
    decode_rgb555le,
    decode_rgb565le,
    resolve_encoding,
    scan_descriptors,
)
from .extraction import extract_table
from .raster import write_bmp, write_png
from .reports import scan_flash
from .scanning import find_descriptor_runs, scan_image_descriptors

__all__ = [name for name in globals() if not name.startswith("_")]
