from __future__ import annotations

import hashlib
import json
import multiprocessing
import os
import struct
from argparse import Namespace
from dataclasses import dataclass
from multiprocessing.pool import Pool
from typing import Iterator


SKEY_COUNT = 100_000_000
_SEARCH_CHUNK_SIZE = 100_000


@dataclass(frozen=True)
class BCoreKeyResult:
    fsn: int
    skey: int
    seed: bytes
    bootkey: bytes
    bcore_hash: bytes

    def as_dict(self) -> dict[str, str]:
        return {
            "fsn": f"{self.fsn:08X}",
            "skey": f"{self.skey:08d}",
            "seed": self.seed.hex(),
            "bootkey": self.bootkey.hex(),
            "bcore_hash": self.bcore_hash.hex(),
        }


def parse_fsn(value: str) -> int:
    text = value.removeprefix("0x").removeprefix("0X")
    if len(text) != 8:
        raise ValueError("FSN must be exactly eight hexadecimal digits")
    try:
        fsn = int(text, 16)
    except ValueError as exc:
        raise ValueError("FSN must be exactly eight hexadecimal digits") from exc
    return fsn


def parse_skey(value: str) -> int:
    if len(value) != 8 or not value.isascii() or not value.isdecimal():
        raise ValueError("SKey must be exactly eight decimal digits")
    return int(value, 10)


def parse_bcore_hash(value: str) -> bytes:
    if len(value) != 32:
        raise ValueError("BCORE hash must be exactly 32 hexadecimal digits")
    try:
        result = bytes.fromhex(value)
    except ValueError as exc:
        raise ValueError(
            "BCORE hash must be exactly 32 hexadecimal digits"
        ) from exc
    if len(result) != 16:
        raise ValueError("BCORE hash must be exactly 32 hexadecimal digits")
    return result


def _validate_fsn(fsn: int) -> None:
    if not isinstance(fsn, int) or not 0 <= fsn <= 0xFFFFFFFF:
        raise ValueError("FSN must fit in an unsigned 32-bit value")


def _validate_skey(skey: int) -> None:
    if not isinstance(skey, int) or not 0 <= skey < SKEY_COUNT:
        raise ValueError("SKey must be in the range 00000000..99999999")


def fold_seed(seed: bytearray) -> None:
    if len(seed) != 16:
        raise ValueError("seed buffer must be exactly 16 bytes")
    for index in range(8):
        seed[index + 8] = seed[index] ^ seed[index + 3]


def build_seed(fsn: int, skey: int) -> bytes:
    _validate_fsn(fsn)
    _validate_skey(skey)
    seed = bytearray(struct.pack("<II", fsn, skey) + bytes(8))
    fold_seed(seed)
    return bytes(seed)


def hash_bootkey(bootkey: bytes) -> bytes:
    if len(bootkey) != 16:
        raise ValueError("BOOTKEY must be exactly 16 bytes")
    return hashlib.md5(bootkey).digest()


def derive_bcore_key(fsn: int, skey: int) -> BCoreKeyResult:
    seed = build_seed(fsn, skey)
    bootkey = hashlib.md5(seed).digest()
    return BCoreKeyResult(
        fsn=fsn,
        skey=skey,
        seed=seed,
        bootkey=bootkey,
        bcore_hash=hash_bootkey(bootkey),
    )


def _search_chunk(arguments: tuple[int, bytes, int, int]) -> int | None:
    fsn, target_hash, start, stop = arguments
    seed = bytearray(16)
    struct.pack_into("<I", seed, 0, fsn)
    for skey in range(start, stop):
        struct.pack_into("<I", seed, 4, skey)
        fold_seed(seed)
        bootkey = hashlib.md5(seed).digest()
        if hashlib.md5(bootkey).digest() == target_hash:
            return skey
    return None


def _search_chunks(fsn: int, target_hash: bytes) -> Iterator[
    tuple[int, bytes, int, int]
]:
    for start in range(0, SKEY_COUNT, _SEARCH_CHUNK_SIZE):
        yield fsn, target_hash, start, min(start + _SEARCH_CHUNK_SIZE, SKEY_COUNT)


def _default_workers() -> int:
    return min(os.cpu_count() or 1, 8)


def recover_bcore_key(
    fsn: int, target_hash: bytes, *, workers: int | None = None
) -> BCoreKeyResult | None:
    _validate_fsn(fsn)
    if len(target_hash) != 16:
        raise ValueError("BCORE hash must be exactly 16 bytes")
    worker_count = _default_workers() if workers is None else workers
    if not isinstance(worker_count, int) or worker_count < 1:
        raise ValueError("workers must be a positive integer")

    chunks = _search_chunks(fsn, target_hash)
    if worker_count == 1:
        for chunk in chunks:
            skey = _search_chunk(chunk)
            if skey is not None:
                return derive_bcore_key(fsn, skey)
        return None

    pool: Pool = multiprocessing.get_context().Pool(worker_count)
    pool_active = True
    try:
        for skey in pool.imap(_search_chunk, chunks, chunksize=1):
            if skey is not None:
                pool.terminate()
                pool.join()
                pool_active = False
                return derive_bcore_key(fsn, skey)
        pool.close()
        pool.join()
        pool_active = False
    finally:
        if pool_active:
            pool.terminate()
            pool.join()
    return None


def _print_result(result: BCoreKeyResult, *, json_output: bool) -> None:
    data = result.as_dict()
    if json_output:
        print(json.dumps(data, indent=2, sort_keys=True))
        return
    print(f"FSN:        {data['fsn']}")
    print(f"SKey:       {data['skey']}")
    print(f"Seed:       {data['seed']}")
    print(f"BOOTKEY:    {data['bootkey']}")
    print(f"BCORE hash: {data['bcore_hash']}")


def command_bcore_key_derive(args: Namespace) -> None:
    _print_result(
        derive_bcore_key(args.fsn, args.skey), json_output=args.json
    )


def command_bcore_key_recover(args: Namespace) -> None:
    result = recover_bcore_key(args.fsn, args.hash, workers=args.workers)
    if result is None:
        raise ValueError("no SKey in 00000000..99999999 matches the BCORE hash")
    _print_result(result, json_output=args.json)
