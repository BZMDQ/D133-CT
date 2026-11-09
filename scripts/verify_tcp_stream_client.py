"""本机模拟设备测试：不连接开发板，不执行Flash写入。"""
import hashlib,json,socket,struct,sys,threading,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'toolbox'/'Source'))
import d133_ota_client as client
from local_tcp_client import frame

def exact(s,n):
 b=b''
 while len(b)<n:
  part=s.recv(n-len(b))
  if not part:raise EOFError()
  b+=part
 return b

def test(path,capability=True,bad_ack=False):
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(2);port=listener.getsockname()[1]
 seen=[];failures=[]
 def server():
  try:
   with listener.accept()[0] as s:
    received=bytearray();total=0
    while True:
     h=exact(s,20);_,op,offset,n,crc=struct.unpack('<4sIIII',h);body=exact(s,n);seen.append(op)
     if op==client.INFO:
      info={'product':client.PRODUCT,'hardware':client.HARDWARE,'version':'1.0.0','protocol':1,'slot':'A','confirmed':True,'next_slot':'A'}
      if capability:info['stream_ota']=2
      answer=json.dumps(info).encode()
     elif op==client.STREAM_BEGIN:
      assert len(body)==388 and body[132:]==Path(str(path)+'.sig').read_bytes();total=struct.unpack_from('<I',body)[0];expected=body[4:36];answer=b''
     elif op==client.DATA:
      assert offset==len(received);received.extend(body);answer=struct.pack('<I',len(received)+(1 if bad_ack else 0))
     elif op==client.COMMIT:
      assert len(received)==total and hashlib.sha256(received).digest()==expected;answer=struct.pack('<I',total)
     else:raise AssertionError('不允许的命令 '+str(op))
     s.sendall(frame(op,0,answer))
     if op==client.COMMIT or bad_ack and op==client.DATA:break
   if capability and not bad_ack:
    with listener.accept()[0] as s:
     h=exact(s,20);op=struct.unpack_from('<I',h,4)[0];assert op==client.HEALTH
     state={'version':client.validate_cpio(Path(path).read_bytes()),'confirmed':True,'slot':'B','next_slot':'B'}
     s.sendall(frame(op,0,json.dumps(state).encode()))
  except EOFError:pass
  except BaseException as e:failures.append(repr(e))
  finally:listener.close()
 task=threading.Thread(target=server,daemon=True);task.start()
 error=None
 try:client.upload('127.0.0.1',port,path,lambda *_:None,threading.Event())
 except Exception as e:error=str(e)
 task.join(5);assert not task.is_alive(), '模拟服务未退出';assert not failures,failures
 assert client.SIGNATURE not in seen,'签名必须在BEGIN前发送'
 if not capability:assert error and client.STREAM_BEGIN not in seen and client.COMMIT not in seen
 elif bad_ack:assert error and client.COMMIT not in seen
 else:assert error is None and client.COMMIT in seen,error
 print('PASS',capability,bad_ack)

if __name__=='__main__':
 for cap,bad in [(True,False),(False,False),(True,True)]:test(sys.argv[1],cap,bad)
