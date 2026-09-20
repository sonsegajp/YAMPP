"""Apply YAMPP's two pinned HSDLib action-compiler fixes; refuse other changes."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = Path(__file__).parent / 'patches/hsdlib-action-compiler.patch'
MANIFEST = PATCH.with_suffix('.json')


def normalized(raw):
    return raw.replace(b'\r\n', b'\n')


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def sections():
    result = {}
    current = None
    for line in PATCH.read_text(encoding='utf-8').splitlines(keepends=True):
        if line.startswith('--- a/'):
            current = line[6:].rstrip('\n')
            if current in result:
                raise ValueError('Duplicate file in HSDLib patch')
            result[current] = []
        elif line.startswith('+++ b/'):
            if line[6:].rstrip('\n') != current:
                raise ValueError('Unexpected rename in HSDLib patch')
        elif current is not None:
            result[current].append(line)
        else:
            raise ValueError('Unexpected HSDLib patch header')
    return result


def apply_hunks(raw, patch):
    source = raw.decode('utf-8').splitlines(keepends=True)
    output = []
    position = index = 0
    while index < len(patch):
        match = re.fullmatch(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n', patch[index])
        if not match:
            raise ValueError('Invalid HSDLib patch hunk')
        start, count, _, new_count = (int(value) if value is not None else 1 for value in match.groups())
        start = max(0, start - 1)
        if start < position or start > len(source):
            raise ValueError('Invalid HSDLib patch position')
        output.extend(source[position:start])
        position = start
        removed = added = 0
        index += 1
        while index < len(patch) and not patch[index].startswith('@@ '):
            line = patch[index]
            if not line or line[0] not in ' +-':
                raise ValueError('Invalid HSDLib patch line')
            if line[0] in ' -':
                if position >= len(source) or source[position] != line[1:]:
                    raise ValueError('HSDLib patch context mismatch')
                position += 1
                removed += 1
            if line[0] in ' +':
                output.append(line[1:])
                added += 1
            index += 1
        if removed != count or added != new_count:
            raise ValueError('HSDLib patch hunk length mismatch')
    output.extend(source[position:])
    return ''.join(output).encode('utf-8')


def patch_checkout(checkout):
    checkout = Path(checkout).resolve()
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    patches = sections()
    if set(patches) != set(manifest['files']):
        raise ValueError('HSDLib patch file list does not match its manifest')
    pending = []
    result = []
    # Validate both complete files before writing either. Already-patched files
    # remain byte-for-byte untouched, including their existing line endings.
    for name, hashes in manifest['files'].items():
        path = checkout / name
        if path.is_symlink() or not path.resolve().is_relative_to(checkout):
            raise ValueError('Unsafe HSDLib source path: ' + name)
        original = normalized(path.read_bytes())
        source_hash = digest(original)
        if source_hash == hashes['patched']:
            result.append({'file': name, 'status': 'already patched'})
            continue
        if source_hash != hashes['original']:
            raise ValueError('HSDLib source differs from pinned original/patched files: ' + name)
        modified = apply_hunks(original, patches[name])
        if digest(modified) != hashes['patched']:
            raise ValueError('HSDLib patched source hash mismatch: ' + name)
        pending.append((path, original, modified))
        result.append({'file': name, 'status': 'patched'})
    for path, original, _ in pending:
        if normalized(path.read_bytes()) != original:
            raise ValueError('HSDLib source changed while preparing its patch')
    for path, _, modified in pending:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.yampp-', suffix='.tmp', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(modified)
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return {'commit': manifest['commit'], 'files': result}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'upstream/HSDLib')
    args = parser.parse_args()
    try:
        print(json.dumps(patch_checkout(args.source), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
