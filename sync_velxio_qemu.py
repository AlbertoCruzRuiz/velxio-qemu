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
PROTECTED = {'.git', '.github', '.gitignore', '.gitmodules',
             'maintenance', 'tests', 'upstream-version.json', 'upstream-files.json'}


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
            values = [node.get(k) for k in ('name', 'asset', 'id', 'slug', 'key', 'filename')]
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
                timestamp = None
                for k in ('published_at', 'created_at', 'uploaded_at', 'updated_at',
                          'date', 'timestamp', 'published', 'created'):
                    timestamp = date(node.get(k))
                    if timestamp:
                        break
                entry = {'name': name, 'sha256': h, 'date': timestamp, 'order': len(ordered)}
                found[name] = entry
                ordered.append(entry)
                return
            for key, value in node.items():
                if isinstance(value, (list, dict)):
                    walk(value, key)
    walk(obj)
    if not ordered:
        raise ValueError('Manifest does not contain qemu-source-<commit> with sha256')
    return ordered


def latest(assets, override):
    if override:
        matches = [a for a in assets if a['name'] == override]
        if not matches:
            raise ValueError('Requested source asset not present in manifest')
        return matches[0]
    if all(a['date'] is not None for a in assets):
        return max(assets, key=lambda a: (a['date'], -a['order']))
    # Fallback only: visually the official listing is newest first.
    print('WARNING: missing timestamps; assuming the manifest is newest-first')
    return assets[0]


def parts_for(name):
    if not name or name.startswith('/') or '\\' in name or '\x00' in name:
        raise ValueError('Invalid archive path')
    parts = PurePosixPath(name).parts
    if '..' in parts:
        raise ValueError('Parent traversal in archive')
    return tuple(p for p in parts if p not in ('', '.'))


def is_allowed(parts):
    return bool(parts) and parts[0] not in PROTECTED and not parts[0].startswith('.git')


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
        total = 0
        symlinks = set()
        for m in members:
            full = parts_for(m.name)
            if full[:len(prefix)] != prefix:
                continue
            rel = full[len(prefix):]
            if not is_allowed(rel):
                continue
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


def import_tree(stage, paths):
    old = set()
    if TRACKED.exists():
        prior = json.loads(TRACKED.read_text(encoding='utf-8'))
        if not isinstance(prior, list) or any(not isinstance(p, str) for p in prior):
            raise ValueError('Invalid upstream-files.json')
        old = set(prior)
    else:
        print('First sync: will not delete older source files without a baseline manifest')

    for path in sorted(old - paths):
        parts = parts_for(path)
        if is_allowed(parts):
            dest = Path(*parts)
            if dest.is_file() or dest.is_symlink():
                dest.unlink()
    for path in sorted(paths):
        source, dest = stage / path, Path(path)
        if any(parent.is_symlink() for parent in list(dest.parents)[:-1]):
            raise ValueError(f'Unexpected symlink in destination parent for {path}')
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.is_symlink() or dest.is_file():
            dest.unlink()
        elif dest.exists():
            raise ValueError(f'File/directory conflict for {path}')
        if source.is_symlink():
            dest.symlink_to(os.readlink(source))
        else:
            shutil.copy2(source, dest, follow_symlinks=False)

    TRACKED.write_text(json.dumps(sorted(paths), indent=2) + '\n', encoding='utf-8')


def main():
    key = os.getenv('VELXIO_LICENSE_KEY')
    if not key:
        raise ValueError('VELXIO_LICENSE_KEY repository secret is missing')
    override = os.getenv('SOURCE_ASSET', '').strip()
    if override and not ASSET.fullmatch(override):
        raise ValueError('SOURCE_ASSET must have the form qemu-source-<commit>')
    current = json.loads(VERSIONS.read_text(encoding='utf8')) if VERSIONS.exists() else {}
    previous = current.get('source_asset') or (
        'qemu-source-' + current['qemu_commit'] if current.get('qemu_commit') else '')

    with tempfile.TemporaryDirectory(prefix='velxio-qemu-update-') as d:
        tmp = Path(d)
        manifest = tmp / 'manifest.json'
        api_get(None, key, manifest)
        available = manifest_assets(json.loads(manifest.read_text(encoding='utf8')))
        chosen = latest(available, override)
        if previous == chosen['name'] and Path('build_libqemu-esp32.sh').is_file():
            checksum = current.get('verified_sha256') or current.get('sha256')
            if checksum and checksum.lower() != chosen['sha256']:
                raise ValueError('Published SHA-256 changed for an existing release')
            print('Already on newest source:', chosen['name'])
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
        print('Downloading', chosen['name'], 'SHA256:', chosen['sha256'])
        api_get(chosen['name'], key, tarball)
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
        print('Imported', len(paths), 'source files; PR can now be prepared')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, RuntimeError, tarfile.TarError, json.JSONDecodeError) as err:
        # Avoid URLs in exceptions, since the query contains a license key.
        print('ERROR:', str(err)[:250], file=sys.stderr)
        sys.exit(1)
