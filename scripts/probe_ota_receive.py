"""仅诊断SD暂存接收，不发送签名、COMMIT、重启或Flash写入指令。"""
import argparse,csv,hashlib,json,socket,struct,sys,time,threading
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'toolbox'/'Source'))
from local_tcp_client import frame,receive

def request(sock,op,offset=0,payload=b''):
    sock.sendall(frame(op,offset,payload))
    returned,body=receive(sock)
    if returned!=op:raise RuntimeError('应答命令不匹配')
    return body

def monitor(args,stop):
    # 独立连接只查询状态，与下载连接不共享读写缓存。
    with Path(args.log+'.health.jsonl').open('w',encoding='utf-8') as output:
        while not stop.is_set():
            started=time.monotonic()
            try:
                with socket.create_connection((args.host,5000),3) as query:
                    query.settimeout(3)
                    state=json.loads(request(query,0xD105))
                record={'elapsed_ms':round((time.monotonic()-started)*1000),'state':state}
            except Exception as error:record={'error':str(error)}
            output.write(json.dumps(record,ensure_ascii=False)+'\n');output.flush()
            stop.wait(1)

def run(args):
    data=Path(args.package).read_bytes()
    with Path(args.log).open('w',encoding='utf-8',newline='') as output:
        log=csv.writer(output);log.writerow(['offset','bytes','elapsed_ms','result'])
        with socket.create_connection((args.host,5000),5) as sock:
            sock.settimeout(20);sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            info=json.loads(request(sock,0xD105))
            if info.get('product')!='D133CBS':raise RuntimeError('不是D133CBS')
            metadata=struct.pack('<I32s32s32s32s',len(data),hashlib.sha256(data).digest(),b'D133CBS',b'D133CBV-QFN88-V1-2',b'1.0.999')
            request(sock,0xD101,payload=metadata)
            stop=threading.Event();watch=threading.Thread(target=monitor,args=(args,stop),daemon=True);watch.start()
            for offset in range(0,len(data),args.chunk):
                block=data[offset:offset+args.chunk];started=time.monotonic()
                try:
                    ack=request(sock,0xD102,offset,block)
                    if ack!=struct.pack('<I',offset+len(block)):raise RuntimeError('进度不匹配')
                except Exception as error:
                    log.writerow([offset,len(block),round((time.monotonic()-started)*1000),str(error)]);output.flush();stop.set();watch.join(4);raise
                log.writerow([offset,len(block),round((time.monotonic()-started)*1000),'ok']);output.flush()
            stop.set();watch.join(4)
            print('SD暂存接收完成；未发送签名、COMMIT或重启命令，不会调用官方Flash升级。')
    print('诊断结束后设备因连接关闭显示接收失败属于预期，现有系统槽不变。')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',required=True);parser.add_argument('--package',required=True)
    parser.add_argument('--log',required=True);parser.add_argument('--chunk',type=int,choices=[4096,8192,16384],default=16384)
    parser.add_argument('--receive-only',action='store_true',required=True)
    run(parser.parse_args())
