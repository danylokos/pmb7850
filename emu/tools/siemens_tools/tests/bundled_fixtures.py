"""Temporary catalogs/maps derived from immutable bundled images for integration tests."""
from __future__ import annotations

import atexit
import json
import tempfile
from functools import lru_cache
from pathlib import Path

from tools.bundled_firmware import ROOT, entries, image, verify
from tools.siemens_tools import layout
from tools.siemens_tools.eeprom import load_eeprom_source
from tools.siemens_tools.fullflash.corpus import build_corpus
from tools.siemens_tools.fullflash.official_corpus import build_official_corpus
from tools.siemens_tools.fullflash.assembly import load_assembly_context as load_context
from .firmware_fixtures import build_xbi

_TEMP = tempfile.TemporaryDirectory(prefix="bundled-firmware-tests-")
atexit.register(_TEMP.cleanup)
TEMP = Path(_TEMP.name)


def catalogs(model: str) -> Path:
    return _catalogs(model.upper())


@lru_cache(maxsize=None)
def _catalogs(model: str) -> Path:
    root = TEMP / model
    loaded = layout.load_layout(model)
    packages = root / "packages"
    packages.mkdir(parents=True)
    sources = []
    for item in entries(model):
        verify(item)
        source = ROOT / item["image"]
        data = source.read_bytes()
        sources.append(source)
        # The official catalog API accepts XBI packages. Wrap the bundled
        # region bytes with the existing format fixture; no original updater.
        package = packages / f'{item["name"]}.xbi'
        package.write_bytes(build_xbi(
            version=32, flash_size=loaded.layout.length, model=model.encode(),
            svn=item["software_version"],
            langpack=f'LG{item["langpack"]}'.encode(), t9=item["t9_version"],
            writes=[(offset, data[offset:offset + 0x8000])
                    for offset in range(0, len(data), 0x8000)],
        ))
    official = root / "official"
    doc = build_official_corpus([packages], loaded, materialize=True, corpus_root=official)
    (official / "catalog.json").write_text(json.dumps(doc))
    community = root / "community"
    doc = build_corpus(sources, loaded, write_splits=True, corpus_root=community,
                       official_catalog=official / "catalog.json")
    (community / "catalog.json").write_text(json.dumps(doc))
    return root


def context(model: str, *args, **kwargs):
    root = catalogs(model)
    return load_context(model, *(args or (root / "community", root / "official")), **kwargs)


@lru_cache(maxsize=None)
def eeprom_map(model: str) -> Path:
    """Map-format representation of non-identity records, not factory evidence."""
    _, blocks = load_eeprom_source(image(model))
    product = {"c55": 130, "a55": 196, "m55": 86}[model.lower()]
    item = entries(model)[0]
    text = [f'[MapFileInfo]\nProduct = {product}\nSWVersion = {item["software_version"]}\n']
    from tools.siemens_tools.eeprom import get_eeprom_profile
    profile = get_eeprom_profile(f"{model.lower()}-emulator-minimal-v1")
    supplied = {record.block_id for record in profile.records}
    supplied.update(profile.donor_blocks)
    supplied.update((76, 5008, 5009, 5077))
    for bid, block in sorted(blocks.items()):
        if bid in supplied:
            continue
        payload = ' '.join(f'0x{x:02X}' for x in block.payload)
        text.append(f'[{bid}]\nMemory = {block.memory_class}\nVersion = {block.version}\nDataSize = {block.length}\nData {{ {payload} }}\n')
    path = TEMP / f'{model}.map'
    path.write_text('\n'.join(text))
    return path


def ambiguous_t9(context, source="community"):
    """Two bundled payloads with deliberately equal scope for ambiguity checks."""
    from dataclasses import replace
    from tools.siemens_tools.fullflash.assembly import region_candidates
    candidates = region_candidates(context, source, "T9")
    target = next(x for x in candidates if x.scope.get("langpack") == "lg91")
    other = next(x for x in candidates if x.sha256 != target.sha256)
    return [target, replace(other, scope=target.scope)]
