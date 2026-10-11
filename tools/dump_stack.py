import sys, struct
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1])
r = mf.get_reader()
mods = [(m.baseaddress, m.baseaddress+m.size, m.name.split(chr(92))[-1]) for m in mf.modules.modules]
def mod(a):
    for b,e,n in mods:
        if b<=a<e: return n, a-b
    return None
exc = mf.exception.exception_records[0]
tid = exc.ThreadId
print('exc', exc.ExceptionRecord.ExceptionCode, hex(exc.ExceptionRecord.ExceptionAddress), mod(exc.ExceptionRecord.ExceptionAddress))
th = [t for t in mf.threads.threads if t.ThreadId==tid][0]
ctx = exc.ThreadContext
import minidump.streams.ThreadListStream as T
rsp = None
# read context from exception's ThreadContext location
buf = mf.file_handle if hasattr(mf,'file_handle') else None
f = open(sys.argv[1],'rb'); f.seek(ctx.Rva); c = f.read(ctx.DataSize)
rsp = struct.unpack_from('<Q', c, 0x98)[0]
print('rsp', hex(rsp))
data=b''
for k in range(0,0x6000,0x100):
    try: data+=r.read(rsp+k,0x100)
    except Exception: break
for i in range(0, len(data)-8, 8):
    v = struct.unpack_from('<Q', data, i)[0]
    m = mod(v)
    if m:
        print(hex(rsp+i), m[0], hex(m[1]))
