"""Strict streaming reader for the inspected AutoHack Interface CSV schema."""
from contextlib import ExitStack
import csv
import hashlib
from pathlib import Path
import re
from zipfile import ZipFile, is_zipfile

from .model import Frame, Record

DATA_HEADER = ['Interface', 'Timestamp', 'Arbitration_ID', 'DLC', 'Data']
HEX_ID = re.compile(r'^(?:0[xX])?[0-9A-Fa-f]+$')
HEX_BYTE = re.compile(r'^[0-9A-Fa-f]{2}$')


def file_sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def timestamp_us(text):
    whole, dot, fraction = text.partition('.')
    if not whole.isdigit() or (dot and not fraction.isdigit()):
        raise ValueError('timestamp must be fixed decimal seconds')
    if len(fraction) > 6 and any(c != '0' for c in fraction[6:]):
        raise ValueError('timestamp exceeds exact microsecond precision')
    return int(whole) * 1_000_000 + int(fraction[:6].ljust(6, '0') or '0')


class Source:
    def __init__(self, path, member=None):
        self.path = Path(path).resolve()
        self.member = member
        self.metadata = {'input': str(self.path), 'member': member, 'complete': False, 'rows': 0}
        self._stack = None
        self._started = False

    def __enter__(self):
        self._stack = ExitStack()
        try:
            self._initial = self.path.stat()
            self.metadata['input_sha256'] = file_sha256(self.path)
            if self.member is not None:
                archive = self._stack.enter_context(ZipFile(self.path))
                if archive.getinfo(self.member).is_dir():
                    raise ValueError('CSV member is a directory')
                self._raw = self._stack.enter_context(archive.open(self.member))
            else:
                if is_zipfile(self.path):
                    raise ValueError('ZIP input requires --member')
                self._raw = self._stack.enter_context(self.path.open('rb'))
        except BaseException:
            self._stack.close()
            raise
        return self

    def __exit__(self, *args):
        return self._stack.__exit__(*args)

    def __iter__(self):
        if self._started or self._stack is None:
            raise ValueError('Source must be opened and consumed exactly once')
        self._started = True
        h = hashlib.sha256()
        frames_hash = hashlib.sha256()

        def lines():
            first = True
            for raw in self._raw:
                h.update(raw)
                yield raw.decode('utf-8-sig' if first else 'utf-8')
                first = False

        reader = csv.reader(lines(), strict=True)
        try:
            header = next(reader, None)
            if header not in (DATA_HEADER, DATA_HEADER + ['Label']):
                raise ValueError('expected AutoHack Interface CSV header')
            with_labels = len(header) == 6
            self.metadata['has_labels'] = with_labels
            for row in reader:
                if len(row) != len(header):
                    raise ValueError('CSV row width differs from header')
                bus, text_time, text_id, text_dlc, text_data = row[:5]
                if not HEX_ID.fullmatch(text_id):
                    raise ValueError('Arbitration_ID must be hexadecimal')
                if not text_dlc.isdigit():
                    raise ValueError('DLC must be an integer')
                if any(not HEX_BYTE.fullmatch(token) for token in text_data.split()):
                    raise ValueError('Data must contain two-digit HEX bytes')
                f = Frame(bus, timestamp_us(text_time), int(text_id, 16), int(text_dlc), bytes.fromhex(text_data))
                label = row[5] if with_labels else None
                if with_labels and not label:
                    raise ValueError('missing truth label')
                frames_hash.update(f'{f.bus}:{f.timestamp_us}:{f.can_id}:{f.dlc}:{f.data.hex()}\n'.encode('ascii'))
                self.metadata['rows'] += 1
                yield Record(f, reader.line_num, label)
        except (ValueError, csv.Error, UnicodeError) as exc:
            raise ValueError(f'{self.member or self.path.name}:line {reader.line_num}: {exc}') from exc
        current = self.path.stat()
        if (current.st_size, current.st_mtime_ns) != (self._initial.st_size, self._initial.st_mtime_ns):
            raise ValueError('source changed during reading')
        self.metadata.update(member_sha256=h.hexdigest(), frames_sha256=frames_hash.hexdigest(), complete=True)
