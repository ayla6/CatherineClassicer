import sys,struct
d=open(sys.argv[1],'rb').read()
hs=struct.unpack('>I',d[12:16])[0]*2048 if False else 0x800
i=0x800; seen={}
while i < len(d)-6 and len(seen)<40:
    if d[i:i+3]!=b'\0\0\1': i+=1; continue
    sid=d[i+3]
    if sid==0xBA: i+=14+(d[i+13]&7); continue
    L=struct.unpack('>H',d[i+4:i+6])[0]
    if sid==0xBD:
        hl=d[i+8]; p=d[i+9+hl:i+6+L]
        k=len(seen); seen[k]=1; print(hex(i),L,p[:24].hex())
    i+=6+L
