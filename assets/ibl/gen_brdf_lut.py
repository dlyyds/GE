# 标准 GGX split-sum BRDF LUT（Karis 式，Schlick-Smith G，稳定可验证）
#   R=dfg1(F0系数), G=dfg2(菲涅尔尾)；u=NoV(0左), v=roughness(0顶)
#   shader: brdf=texture(lut,vec2(NoV,roughness)).rg; spec=env*(F*brdf.r+brdf.g)
import numpy as np, math
from PIL import Image
SIZE, SAMPLES = 128, 4096
def radical_inverse_vdc(bits):
    v=np.uint32(bits)
    v=((v<<16)|(v>>16))&0xFFFFFFFF
    v=((v&0x55555555)<<1)|((v&0xAAAAAAAA)>>1)
    v=((v&0x33333333)<<2)|((v&0xCCCCCCCC)>>2)
    v=((v&0x0F0F0F0F)<<4)|((v&0xF0F0F0F0)>>4)
    v=((v&0x00FF00FF)<<8)|((v&0xFF00FF00)>>8)
    return float(v)/2**32
idxs=np.arange(SAMPLES,dtype=np.uint32)
xi_x=idxs/SAMPLES
xi_y=np.array([radical_inverse_vdc(int(i)) for i in idxs])
def g_schlick(ndotv,rough):
    r=rough+1.0; k=(r*r)/8.0
    return ndotv/(ndotv*(1.0-k)+k)
def gen(NoV,roughness):
    a=roughness*roughness
    phi=2.0*math.pi*xi_x
    cosT=np.sqrt((1.0-xi_y)/(1.0+(a*a-1.0)*xi_y))
    sinT=np.sqrt(np.maximum(1.0-cosT*cosT,0.0))
    Hx=np.cos(phi)*sinT; Hy=np.sin(phi)*sinT; Hz=cosT
    sin_v=math.sqrt(max(1.0-NoV*NoV,0.0))
    V=np.array([sin_v,0.0,NoV])
    VoH=np.clip(Hx*V[0]+Hz*V[2],0.0,1.0)
    L=(2.0*VoH*Hx-V[0], 2.0*VoH*Hy, 2.0*VoH*Hz-V[2])
    NoL=np.clip(L[2],0.0,1.0); NoH=np.clip(Hz,0.0,1.0)
    G=g_schlick(NoV,roughness)*g_schlick(NoL,roughness)
    denom=NoH*NoV
    G_vis=np.where(denom>1e-6, G*VoH/denom, 0.0)
    Fc=np.power(np.clip(1.0-VoH,0.0,1.0),5.0)
    dfg1=np.sum((1.0-Fc)*G_vis)/SAMPLES
    dfg2=np.sum(Fc*G_vis)/SAMPLES
    return dfg1,dfg2
lut=np.zeros((SIZE,SIZE,3),dtype=np.uint8)
for vy in range(SIZE):
    roughness=vy/(SIZE-1)
    for vx in range(SIZE):
        NoV=vx/(SIZE-1)
        d1,d2=gen(NoV,roughness)
        lut[vy,vx,0]=int(round(min(max(d1,0),1)*255))
        lut[vy,vx,1]=int(round(min(max(d2,0),1)*255))
Image.fromarray(lut,'RGB').save('brdf_lut.png')
im=Image.open('brdf_lut.png')
def px(NoV,rough):
    r,g,_=im.getpixel((int(NoV*(SIZE-1)),int(rough*(SIZE-1)))); return r/255,g/255
print('rough=0 dfg1:', [round(px(n,0)[0],3) for n in [0,0.25,0.5,0.75,1]])
print('rough=0 dfg2:', [round(px(n,0)[1],3) for n in [0,0.25,0.5,0.75,1]])
print('rough=1 dfg1:', [round(px(n,1)[0],3) for n in [0,0.25,0.5,0.75,1]])
print('rough=0.25 dfg1:', [round(px(n,0.25)[0],3) for n in [0,0.25,0.5,0.75,1]])
