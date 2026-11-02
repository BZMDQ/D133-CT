"""离线验证官方 CPIO 包白名单、成员加和与 SPL 防护，不连接开发板。"""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'toolbox'/'Source'))
from d133_ota_client import validate_cpio

def member(data,wanted):
    pos=0
    while pos+110<=len(data):
        h=data[pos:pos+110];size=int(h[54:62],16);names=int(h[94:102],16)
        name=data[pos+110:pos+110+names].rstrip(b'\0');start=(pos+110+names+3)&~3
        if name==wanted:return pos,start,size
        if name==b'TRAILER!!!':break
        pos=(start+size+3)&~3
    raise AssertionError('CPIO member missing')

def rejected(data):
    try:validate_cpio(data)
    except ValueError:return True
    return False

def main(path):
    package=bytearray(Path(path).read_bytes());version=validate_cpio(package)
    assert version and len(package)>512
    # 内容被改而没有更新 CPIO sum 时必须拒绝。
    corrupted=bytearray(package);pos,start,size=member(corrupted,b'd13x_os.itb');corrupted[start+64]^=1
    assert rejected(corrupted),'changed OS bytes must be rejected'
    # 即使攻击者重算 CPIO 加和，也不能把 OS 映射到 SPL。
    malicious=bytearray(package);_,meta_start,_=member(malicious,b'ota_info.bin')
    old=b'd13x_os.itb:os;';new=b'd13x_os.itb:spl;';where=malicious.find(old,meta_start,meta_start+512)
    assert where>=0;malicious[where:where+len(old)]=new
    del malicious[meta_start+511]
    # 更新 ENV 内层 CRC 与 CPIO 成员加和，确保命中目标白名单检查。
    import struct,zlib
    struct.pack_into('<I',malicious,meta_start,zlib.crc32(malicious[meta_start+4:meta_start+512])&0xffffffff)
    cpos,body,size=member(malicious,b'ota_info.bin')
    malicious[cpos+102:cpos+110]=('%08X'%(sum(malicious[body:body+size])&0xffffffff)).encode('ascii')
    assert rejected(malicious),'SPL mapping must be rejected after valid checksums'
    assert rejected(package+b'X'),'trailing payload must be rejected'
    print('OTA host checks passed: version',version,'size',len(package),'; corrupt data, injected SPL mapping and trailing bytes rejected')
if __name__=='__main__':main(sys.argv[1])
