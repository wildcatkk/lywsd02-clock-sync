"""Atomic JSON persistence shared by the web service and the Windows setup."""

import json
import os
import tempfile


def load(path, fallback):
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return dict(fallback)
    # A hand-edited file that parses but is not an object would crash startup.
    if not isinstance(data, dict):
        return dict(fallback)
    merged = dict(fallback)
    merged.update(data)
    return merged


def save(path, data):
    directory = os.path.dirname(os.path.abspath(path))
    handle = tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=directory, prefix=".tmp-", delete=False)
    try:
        json.dump(data, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.flush()
        os.fsync(handle.fileno())
        handle.close()
        os.replace(handle.name, path)
    except BaseException:
        handle.close()
        os.unlink(handle.name)
        raise
