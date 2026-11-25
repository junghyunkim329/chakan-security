"""Truth-free detector input and dataset label normalization."""
from dataclasses import dataclass

BUSES = ('C-CAN', 'P-CAN', 'B-CAN')
RULE_IDS = ('CAN_ID_UNSEEN', 'CAN_DLC_UNSEEN', 'CAN_INTERVAL_SHORT', 'CAN_RATE_HIGH')


@dataclass(frozen=True, slots=True)
class Frame:
    bus: str
    timestamp_us: int
    can_id: int
    dlc: int
    data: bytes

    def __post_init__(self):
        if self.bus not in BUSES:
            raise ValueError('unsupported AutoHack bus')
        if type(self.timestamp_us) is not int or not 0 <= self.timestamp_us <= (1<<63)-1:
            raise ValueError('timestamp must be integer microseconds in 0..INT64_MAX')
        if type(self.can_id) is not int or not 0 <= self.can_id <= 0x7ff:
            raise ValueError('CAN ID must be an 11-bit integer')
        if type(self.dlc) is not int or not 0 <= self.dlc <= 8:
            raise ValueError('DLC must be an integer in 0..8')
        if not isinstance(self.data, bytes) or len(self.data) != self.dlc:
            raise ValueError('payload length must equal DLC')


@dataclass(frozen=True, slots=True)
class Record:
    frame: Frame
    line: int
    label: str | None


def label_family(label: str) -> str:
    if label in {'Normal', 'DoS', 'Fuzzing', 'Spoofing', 'Replay', 'UDS_Spoofing'}:
        return label
    if isinstance(label, str) and label.startswith('Spoofing_UDS_') and len(label) > len('Spoofing_UDS_'):
        return 'UDS_Spoofing'
    raise ValueError(f'unsupported or missing truth label: {label!r}')
