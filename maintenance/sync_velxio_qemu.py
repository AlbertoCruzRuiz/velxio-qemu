#!/usr/bin/env python3
"""Import verified Velxio QEMU GPL source for a human-reviewed PR.

This workflow must NOT execute anything from the downloaded archive.
"""
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

ENDPOINT = 'https://velxio.dev/api/pro/license/downloads'
ASSET = re.compile(r'^qemu-source-([a-fA-F0-9]{7,40})$')
SHA = re.compile(r'^(?:sha256[:=])?([a-fA-F0-9]{64})$')
VERSIONS = Path('upstream-version.json')
TRACKED = Path('upstream-files.json')
HASHES = Path('upstream-hashes.json')
PROTECTED = {
    '.git', '.github', '.gitignore', '.gitmodules',
    'maintenance',
    'upstream-version.json',
    'upstream-files.json',
    'upstream-hashes.json'
}

LOCAL_PROTECTED_FILES = {
    'tests/test_sync_velxio_qemu.py'
}

def emit(name, value):
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf8') as f:
            f.write(f'{name}={value}\n')


def api_get(name, key, dest):
    url = ENDPOINT + ('/' + name if name else '') + '?' + urllib.parse.urlencode({'key': key})
    request = urllib.request.Request(url, headers={'User-Agent': 'velxio-qemu-source-checker/1'})
    try:
        with urllib.request.urlopen(request, timeout=90) as response, dest.open('wb') as f:
            size = 0
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > (250_000_000 if name else 10_000_000):
                    raise ValueError('Download exceeded size limit')
                f.write(chunk)
    except urllib.error.HTTPError as err:
        # Never show the URL: it contains the license in its query string.
        raise ValueError(f'Velxio API returned HTTP {err.code}') from None
    except urllib.error.URLError:
        raise ValueError('Could not reach the Velxio API') from None


def date(value):
    if isinstance(value, (int, float)) and value > 0:
        return dt.datetime.fromtimestamp(value, dt.timezone.utc)
    if isinstance(value, str) and value:
        try:
            d = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
            return (d if d.tzinfo else d.replace(tzinfo=dt.timezone.utc)).astimezone(dt.timezone.utc)
        except ValueError:
            return None
    return None


def manifest_assets(obj):
    """Supports lists, or dictionaries keyed by asset name; fails closed."""
    found = {}
    ordered = []

    def walk(node, outer=None):
        if isinstance(node, list):
            for item in node:
                walk(item)
        elif isinstance(node, dict):
            values = [node.get(k) for k in ('asset_name', 'name', 'asset', 'id', 'slug', 'key', 'filename')]
            values.append(outer)
            raw = next((v for v in values if isinstance(v, str) and
                        ASSET.fullmatch(v.removesuffix('.tar.gz'))), None)
            digest = next((node.get(k) for k in ('sha256', 'sha_256', 'checksum') if node.get(k)), None)
            if raw and digest:
                name = raw.removesuffix('.tar.gz')
                match = SHA.fullmatch(str(digest).strip())
                if not match:
                    raise ValueError(f'Manifest SHA-256 invalid for {name}')
                h = match.group(1).lower()
                if name in found:
                    if found[name]['sha256'] != h:
                        raise ValueError(f'Conflicting checksums for {name}')
                    return
                # Velxio's current API publishes source dates in the 'version'
                # field: '2026-10-15+63b37f5e'. It does not return created_at.
                # Use this authoritative version date rather than JSON array
                # order: the manifest is *not* sorted by publication date.
                version = node.get('version')
                timestamp = None
                if isinstance(version, str):
                    source_version = re.fullmatch(
                        r'(\d{4}-\d{2}-\d{2})\+([a-fA-F0-9]{7,40})', version)
                    if source_version:
                        version_commit = source_version.group(2)
                        if version_commit.lower() != ASSET.fullmatch(name).group(1).lower():
                            raise ValueError(f'Manifest version/commit mismatch for {name}')
                        timestamp = date(source_version.group(1))
                if timestamp is None:
                    for k in ('published_at', 'created_at', 'uploaded_at', 'updated_at',
                              'date', 'timestamp', 'published', 'created'):
                        timestamp = date(node.get(k))
                        if timestamp:
                            break
                size = node.get('size_bytes')
                if size is not None and (not isinstance(size, int) or isinstance(size, bool) or size <= 0):
                    raise ValueError(f'Invalid size_bytes for {name}')
                entry = {'name': name, 'sha256': h, 'date': timestamp,
                         'size_bytes': size, 'order': len(ordered)}
                found[name] = entry
                ordered.append(entry)
                return
            for key, value in node.items():
                if isinstance(value, (list, dict)):
                    walk(value, key)
    walk(obj)
    if not ordered:
        top = sorted(obj.keys()) if isinstance(obj, dict) else type(obj).__name__
        raise ValueError('No qemu-source-<commit> with sha256 in manifest; top-level shape: ' + str(top)[:140])
    return ordered


def latest(assets, override):
    if override:
        matches = [a for a in assets if a['name'] == override]
        if not matches:
            raise ValueError('Requested source asset not present in manifest')
        return matches[0]
    # A silent 'first entry' fallback can select an older QEMU build because
    # the current Velxio manifest lists assets in nonchronological order.
    if any(a['date'] is None for a in assets):
        raise ValueError('Some QEMU sources have no release date; use SOURCE_ASSET '
                         'to select a reviewed revision explicitly')
    newest = max(a['date'] for a in assets)
    matches = [a for a in assets if a['date'] == newest]
    if len(matches) != 1:
        raise ValueError('Several QEMU revisions have the same latest release date; '
                         'use SOURCE_ASSET explicitly')
    return matches[0]


def parts_for(name):
    if not name or name.startswith('/') or '\\' in name or '\x00' in name:
        raise ValueError('Invalid archive path')
    parts = PurePosixPath(name).parts
    if '..' in parts:
        raise ValueError('Parent traversal in archive')
    return tuple(p for p in parts if p not in ('', '.'))


def is_allowed(parts):
    return (
        bool(parts)
        and parts[0] not in PROTECTED
        and not parts[0].startswith('.git')
        and '/'.join(parts) not in LOCAL_PROTECTED_FILES
    )


def unpack(archive, stage):
    """Safely stage a tarball. Ignore any upstream Actions and our own CI files."""
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        if not members or len(members) > 100_000:
            raise ValueError('Invalid archive entry count')
        scripts = [parts_for(m.name) for m in members if m.isfile() and
                   parts_for(m.name)[-1:] == ('build_libqemu-esp32.sh',)]
        if len(scripts) != 1:
            raise ValueError('Cannot locate unique QEMU build script in source tarball')
        prefix = scripts[0][:-1]
        entries = []
        seen = set()
        total = 0
        symlinks = set()
        for m in members:
            full = parts_for(m.name)
            if full[:len(prefix)] != prefix:
                continue
            rel = full[len(prefix):]
            if not is_allowed(rel):
                continue
            if rel in seen:
                raise ValueError('Duplicate paths in QEMU source archive')
            seen.add(rel)
            if not (m.isfile() or m.isdir() or m.issym()):
                raise ValueError('Tar archive contains unsupported link/device type')
            if m.size > 95_000_000:
                raise ValueError('Tar archive contains a file too large for GitHub')
            total += m.size
            if total > 1_500_000_000:
                raise ValueError('Tar archive exceeds expanded size limit')
            entries.append((m, rel))
            if m.issym():
                symlinks.add(rel)

        imported = set()
        for m, rel in entries:
            if any(rel[:k] in symlinks for k in range(1, len(rel))):
                raise ValueError('Tar archive writes beneath a symlink')
            dest = stage.joinpath(*rel)
            if m.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                dest.parent.mkdir(parents=True, exist_ok=True)
                stream = tar.extractfile(m)
                if stream is None:
                    raise ValueError('Cannot read tar archive member')
                with stream, dest.open('wb') as out:
                    shutil.copyfileobj(stream, out)
                dest.chmod(0o755 if m.mode & 0o111 else 0o644)
                imported.add('/'.join(rel))
            elif m.issym():
                link = m.linkname
                if not link or link.startswith('/') or '\\' in link:
                    raise ValueError('Absolute/invalid symlink in archive')
                target = posixpath.normpath(posixpath.join(posixpath.dirname('/'.join(rel)), link))
                if target == '..' or target.startswith('../') or target.startswith('/') or not is_allowed(parts_for(target)):
                    raise ValueError('Tar symlink escapes source directory')
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.symlink_to(link)
                imported.add('/'.join(rel))

        required = {'build_libqemu-esp32.sh', 'configure', 'meson.build'}
        if not required.issubset(imported):
            raise ValueError('Downloaded source missing QEMU configure/meson/build script')
        return imported


def fingerprint(path):
    """Protect locally modified upstream sources against silent overwrite."""
    if path.is_symlink():
        return 'link:' + os.readlink(path)
    if path.is_file():
        h = hashlib.sha256()
        with path.open('rb') as f:
            while chunk := f.read(1024 * 1024):
                h.update(chunk)
        return 'sha256:' + h.hexdigest()
    return None


def import_tree(stage, paths):
    old = set()
    if TRACKED.exists():
        prior = json.loads(TRACKED.read_text(encoding='utf-8'))
        if not isinstance(prior, list) or any(not isinstance(p, str) for p in prior):
            raise ValueError('Invalid upstream-files.json')
        old = set(prior)
    elif not VERSIONS.exists():
        print('BOOTSTRAP: no QEMU sources imported yet; preparing first PR', flush=True)

    if old and not HASHES.exists():
        raise ValueError('Existing import has no upstream-hashes.json baseline. Review/migrate it before updating.')
    hashes = json.loads(HASHES.read_text(encoding='utf-8')) if HASHES.exists() else {}
    if not isinstance(hashes, dict):
        raise ValueError('Invalid upstream-hashes.json')

    # Stop, rather than destroying changes made manually inside the QEMU source.
    for path in sorted(old):
        parts = parts_for(path)
        if not is_allowed(parts):
            continue
        expected = hashes.get(path)
        if expected is None or fingerprint(Path(path)) != expected:
            raise ValueError('Local QEMU source differs from last imported revision: ' + path
                             + '. Rebase your patch manually; nothing has been overwritten.')

    # Check all paths before any update. Reject collisions with maintenance files.
    for path in sorted(paths):
        parts = parts_for(path)
        dest = Path(*parts)
        if not is_allowed(parts):
            raise ValueError('Import would overwrite protected path')
        if any(parent.is_symlink() for parent in list(dest.parents)[:-1]):
            raise ValueError(f'Unexpected symlink in destination parent for {path}')
        if dest.exists() and dest.is_dir() and not dest.is_symlink():
            raise ValueError('File/directory conflict for ' + path)
        if path not in old and (dest.exists() or dest.is_symlink()):
            raise ValueError('Source import collides with an existing non-upstream file: ' + path)

    for path in sorted(old - paths):
        parts = parts_for(path)
        if is_allowed(parts):
            dest = Path(*parts)
            if dest.is_file() or dest.is_symlink():
                dest.unlink()
    new_hashes = {}
    for path in sorted(paths):
        source, dest = stage / path, Path(path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.is_symlink() or dest.is_file():
            dest.unlink()
        elif dest.exists():
            raise ValueError(f'File/directory conflict for {path}')
        if source.is_symlink():
            dest.symlink_to(os.readlink(source))
        else:
            shutil.copy2(source, dest, follow_symlinks=False)
        new_hashes[path] = fingerprint(source)

    TRACKED.write_text(json.dumps(sorted(paths), indent=2) + '\n', encoding='utf-8')
    HASHES.write_text(json.dumps(new_hashes, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def main():
    key = os.getenv('VELXIO_LICENSE_KEY')
    if not key:
        raise ValueError('VELXIO_LICENSE_KEY repository secret is missing')
    override = os.getenv('SOURCE_ASSET', '').strip()
    if override and not ASSET.fullmatch(override):
        raise ValueError('SOURCE_ASSET must have the form qemu-source-<commit>')
    current = json.loads(VERSIONS.read_text(encoding='utf8')) if VERSIONS.exists() else {}
    if not VERSIONS.exists():
        print('INITIAL IMPORT: upstream-version.json does not exist', flush=True)
    previous = current.get('source_asset') or (
        'qemu-source-' + current['qemu_commit'] if current.get('qemu_commit') else '')

    with tempfile.TemporaryDirectory(prefix='velxio-qemu-update-') as d:
        tmp = Path(d)
        manifest = tmp / 'manifest.json'
        print('Fetching Velxio source manifest (credential redacted)', flush=True)
        api_get(None, key, manifest)
        available = manifest_assets(json.loads(manifest.read_text(encoding='utf8')))
        chosen = latest(available, override)
        print(f'Source candidates: {len(available)}; selected: {chosen["name"]}', flush=True)
        if previous and not override and previous not in {a['name'] for a in available}:
            raise ValueError('Previously imported QEMU revision absent from upstream manifest; '
                             'refusing implicit replacement')
        if (previous == chosen['name'] and Path('build_libqemu-esp32.sh').is_file() and Path('tests/meson.build').is_file() and Path('tests/qtest/libqos/meson.build').is_file()):
            checksum = current.get('verified_sha256') or current.get('sha256')
            if checksum and checksum.lower() != chosen['sha256']:
                raise ValueError('Published SHA-256 changed for an existing release')
            print('Already on newest source:', chosen['name'], flush=True)
            emit('changed', 'false')
            return
        previous_asset = next((a for a in available if a['name'] == previous), None)
        if previous_asset and not override:
            if previous_asset['date'] and chosen['date'] and chosen['date'] < previous_asset['date']:
                raise ValueError('Aborting downgrade by date')
            if (not previous_asset['date'] or not chosen['date']) and \
                    previous_asset['order'] < chosen['order']:
                raise ValueError('Aborting downgrade by manifest order')

        tarball = tmp / 'source.tar.gz'
        print('Downloading', chosen['name'], 'expected SHA256:', chosen['sha256'], flush=True)
        api_get(chosen['name'], key, tarball)
        if chosen['size_bytes'] is not None and tarball.stat().st_size != chosen['size_bytes']:
            raise ValueError('Source tarball size_bytes mismatch')
        with tarball.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        if digest != chosen['sha256']:
            raise ValueError('Source tarball SHA-256 mismatch')

        staged = tmp / 'staged'
        staged.mkdir()
        paths = unpack(tarball, staged)
        import_tree(staged, paths)
        commit = ASSET.fullmatch(chosen['name']).group(1)
        VERSIONS.write_text(json.dumps({
            'source': 'velxio.dev (GPL-2.0)',
            'source_asset': chosen['name'],
            'qemu_commit': commit,
            'verified_sha256': chosen['sha256'],
            'published_at': chosen['date'].isoformat() if chosen['date'] else None,
        }, indent=2) + '\n', encoding='utf8')
        emit('changed', 'true')
        emit('asset', chosen['name'])
        emit('sha256', chosen['sha256'])
        print('Imported', len(paths), 'source files; PR can now be prepared', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, RuntimeError, tarfile.TarError, json.JSONDecodeError) as err:
        # Avoid URLs in exceptions, since the query contains a license key.
        print('::error::QEMU source synchronisation failed: ' + str(err)[:250], file=sys.stderr, flush=True)
        sys.exit(1)
