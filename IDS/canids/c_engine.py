"""Historical FFI reference for tests/benchmarks; not the IDS execution path."""
import ctypes as ct
from pathlib import Path
import shutil
import tempfile
import weakref

from .model import BUSES, RULE_IDS, Frame
from .profile import validate_policy
from .source import file_sha256


class CProfile(ct.Structure):
    _fields_=[('bus',ct.c_uint8),('can_id',ct.c_uint16),('allowed_dlcs',ct.c_uint16),
              ('min_interval_us',ct.c_int64),('max_count_1s',ct.c_uint64)]


class CFrame(ct.Structure):
    _fields_=[('bus',ct.c_uint8),('dlc',ct.c_uint8),('can_id',ct.c_uint16),
              ('timestamp_us',ct.c_int64),('data',ct.c_uint8*8)]


class CFinding(ct.Structure):
    _fields_=[('rule',ct.c_uint32),('observed',ct.c_uint64),('threshold',ct.c_uint64)]


def _load_snapshot(path):
    """Unique private path binds the digest to bytes actually passed to dlopen.

    Loading the caller's pathname directly can reuse an old dlopen handle after
    that path is replaced. The owned snapshot also closes the load/hash race.
    """
    storage=tempfile.TemporaryDirectory(prefix='canids-core-')
    try:
        snapshot=Path(storage.name)/'libcanids.so'
        shutil.copyfile(path,snapshot)
        digest=file_sha256(snapshot)
        snapshot.chmod(0o500)
        lib=ct.CDLL(str(snapshot))
        lib.canids_abi_version.argtypes=[];lib.canids_abi_version.restype=ct.c_uint32
        if lib.canids_abi_version()!=1:raise ValueError('unsupported C core ABI')
        for symbol,structure in [('canids_frame_size',CFrame),('canids_profile_size',CProfile),('canids_finding_size',CFinding)]:
            fn=getattr(lib,symbol);fn.argtypes=[];fn.restype=ct.c_size_t
            if fn()!=ct.sizeof(structure):raise ValueError('C core ABI structure size mismatch')
        lib.canids_create.argtypes=[ct.POINTER(CProfile),ct.c_size_t,ct.c_uint64,ct.POINTER(ct.c_int)]
        lib.canids_create.restype=ct.c_void_p
        lib.canids_destroy.argtypes=[ct.c_void_p];lib.canids_destroy.restype=None
        lib.canids_observe.argtypes=[ct.c_void_p,ct.POINTER(CFrame),ct.POINTER(CFinding),ct.c_size_t,ct.POINTER(ct.c_size_t)]
        lib.canids_observe.restype=ct.c_int
        return lib,digest,storage
    except BaseException:
        storage.cleanup()
        raise


def _release(destroy,pointer,storage):
    try:
        destroy(pointer)
    finally:
        storage.cleanup()


class CDetector:
    def __init__(self,policy,library=None):
        p=validate_policy(policy)
        self.policy_hash=p['policy_hash']
        path=Path(library) if library is not None else Path(__file__).resolve().parents[1]/'build/libcanids.so'
        self._library_path=path.resolve()
        lib,digest,storage=_load_snapshot(self._library_path)
        profiles=(CProfile*len(p['profiles']))()
        for target,entry in zip(profiles,p['profiles']):
            target.bus=BUSES.index(entry['bus']);target.can_id=entry['can_id']
            target.allowed_dlcs=sum(1<<d for d in entry['dlcs'])
            target.min_interval_us=-1 if entry['min_interval_us'] is None else entry['min_interval_us']
            target.max_count_1s=entry['max_count_1s']
        status=ct.c_int()
        self._pointer=lib.canids_create(profiles,len(profiles),p['window_us'],ct.byref(status))
        if not self._pointer:
            storage.cleanup()
            if status.value==-3:raise MemoryError('C core allocation failed')
            raise ValueError('C core rejected profile')
        self._lib=lib
        self._finalizer=weakref.finalize(self,_release,lib.canids_destroy,self._pointer,storage)
        self.backend_info={'backend':'c11','abi_version':1,'library':str(self._library_path),
                           'library_sha256':digest,'loaded_from_private_snapshot':True}

    def close(self):
        self._finalizer();self._pointer=None

    def __enter__(self):
        return self

    def __exit__(self,*args):
        self.close()

    def observe(self,frame):
        if self._pointer is None:raise ValueError('C detector is closed')
        if not isinstance(frame,Frame):raise TypeError('Detector accepts only truth-free Frame objects')
        f=CFrame(bus=BUSES.index(frame.bus),dlc=frame.dlc,can_id=frame.can_id,timestamp_us=frame.timestamp_us)
        f.data[:frame.dlc]=frame.data
        out=(CFinding*4)();count=ct.c_size_t()
        status=self._lib.canids_observe(self._pointer,ct.byref(f),out,4,ct.byref(count))
        if status==-2:raise ValueError('timestamp regression during C detection')
        if status==-3:raise MemoryError('C core window allocation failed')
        if status!=0:raise ValueError('C core rejected frame')
        hits=[]
        for item in out[:count.value]:
            rule=RULE_IDS[item.rule-1]
            observed,threshold=item.observed,item.threshold
            if item.rule==1:
                observed=f'0x{frame.can_id:03X}';threshold='ID absent from this bus normal profile'
            elif item.rule==2:
                threshold=[d for d in range(9) if threshold & (1<<d)]
            hits.append({'rule_id':rule,'bus':frame.bus,'can_id':f'0x{frame.can_id:03X}',
                         'source_timestamp_us':frame.timestamp_us,'observed':observed,'threshold':threshold,
                         'window_us':1000000 if item.rule==4 else None})
        return hits
