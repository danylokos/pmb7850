"""Paths and host utilities shared by qualification runners."""
import hashlib
import os
from pathlib import Path
import subprocess


def repository_root() -> Path:
    return Path(__file__).resolve().parents[4]


ROOT = repository_root()
FIXTURE_DIR = Path(__file__).resolve().parents[1] / 'fixtures'


def binary(path):
    path = Path(path).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f'Missing executable: {path}')
    return path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def terminate(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def revision(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'],
                                   text=True, timeout=5).strip()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(root: Path, path: str = ".") -> str:
    completed = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root / path,
                               text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"
