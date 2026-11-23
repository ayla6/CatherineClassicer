import sys,subprocess
def frames(p):
    d=subprocess.run(['ffmpeg','-v','error','-i',p,'-vf','scale=32:18,format=gray','-f','rawvideo','-'],capture_output=True).stdout
    return [d[i:i+576] for i in range(0,len(d),576)]
def dist(a,b): return sum(abs(x-y) for x,y in zip(a,b))/576
E,J=frames(sys.argv[1]),frames(sys.argv[2])
fps=30;i=j=0;extra=[]
while i<len(E) and j<len(J):
    if dist(E[i],J[j])<6: i+=1;j+=1;continue
    # try skipping in J (extra JP frames)
    k=next((k for k in range(1,400) if j+k<len(J) and dist(E[i],J[j+k])<6),None)
    m=next((m for m in range(1,400) if i+m<len(E) and dist(E[i+m],J[j])<6),None)
    if k and (not m or k<=m): extra.append(('JP extra',j/fps,k)); j+=k
    elif m: extra.append(('EN extra',i/fps,m)); i+=m
    else: i+=1;j+=1
print(len(E),len(J))
for e in extra: print("%s at %.2fs: %d frames (%.2fs)"%(e[0],e[1],e[2],e[2]/fps))
