"""修复 SDK mkcpio 回填 ota_info 长度后未同步的两个 CRC，并作严格预检。"""
import argparse, hashlib, json, re, struct, sys, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'toolbox'/'Source'))
from d133_ota_client import validate_cpio, PRODUCT, HARDWARE

def fix_crc_fields(data,version_override=None):
    offset=0
    while offset+110<=len(data):
        head=data[offset:offset+110]
        if head[:6]!=b'070702':raise ValueError('SDK CPIO header is invalid')
        names=int(head[94:102],16);size=int(head[54:62],16)
        raw=data[offset+110:offset+110+names]
        start=(offset+110+names+3)&~3;end=start+size
        if raw.rstrip(b'\0')==b'ota_info.bin':
            if size!=512 or end>len(data):raise ValueError('ota_info.bin must be 512 bytes')
            text=bytes(data[start+4:end]).replace(b'\0',b'\n')
            found=re.search(rb'version\s*=\s*"(\d+\.\d+\.\d+)";',text)
            if not found:raise ValueError('ota_info.bin version is missing')
            current=found.group(1).decode('ascii')
            version=version_override or current
            if not re.fullmatch(r'\d+\.\d+\.\d+',version) or len(version)!=len(current):
                raise ValueError('OTA candidate version must be numeric and preserve ota_info field width')
            if version_override:
                marker=b'version = "'+current.encode()+b'";'
                where=data.find(marker,start+4,end)
                if where<0:raise ValueError('ota_info version location is invalid')
                data[where+len(b'version = "'):where+len(b'version = "')+len(version)]=version.encode('ascii')
            # mkenvimage CRC covers the 508 bytes after its four-byte header.
            struct.pack_into('<I',data,start,zlib.crc32(data[start+4:end])&0xffffffff)
            # ArtInChip ota.c checks the additive CPIO member checksum while unpacking.
            data[offset+102:offset+110]=f'{sum(data[start:end])&0xffffffff:08X}'.encode('ascii')
            return version
        if raw.rstrip(b'\0')==b'TRAILER!!!':break
        offset=(end+3)&~3
    raise ValueError('ota_info.bin was not found in the SDK CPIO')

def prepare(source,output,version=None):
    data=bytearray(Path(source).read_bytes())
    actual=fix_crc_fields(data,version)
    if validate_cpio(data)!=actual:raise ValueError('repaired CPIO version mismatch')
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(data)
    info={'product':PRODUCT,'hardware':HARDWARE,'version':actual,'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'package':output.name,'signature':output.name+'.sig'}
    output.with_suffix('.json').write_text(json.dumps(info,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(info,ensure_ascii=False))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('source');parser.add_argument('output');parser.add_argument('--version');args=parser.parse_args();prepare(args.source,args.output,args.version)
