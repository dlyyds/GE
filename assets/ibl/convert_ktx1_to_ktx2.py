import struct, math

src='assets/ibl/DaySkyHDRI065B_prefilter.ktx'
out='assets/ibl/DaySkyHDRI065B_prefilter.ktx2'

# ---- 天空盒 RGBA16F 的 DFD（92 字节，格式相关、与分辨率无关，直接复用）----
SKY_DFD = bytes.fromhex(
 '5c00000000000000020058000101010000000000080000000000000000000f'
 'c000000000000080bf0000803f10000fc100000000000080bf0000803f2000'
 '0fc200000000000080bf0000803f30000fcf00000000000080bf0000803f')

d=open(src,'rb').read()
kvLen=struct.unpack_from('<I',d,60)[0]
pw,ph=struct.unpack_from('<2I',d,36)
faces=struct.unpack_from('<I',d,52)[0]
levels=struct.unpack_from('<I',d,56)[0]
off=64+kvLen

def decode_minifloat(bits, mbits):
    # 1 符号 + 5 指数 + mbits 尾数（R11G11B10 的 mini-float，指数偏置 15）
    sign = -1.0 if (bits>>(mbits+5)) else 1.0
    e = (bits>>mbits) & 0x1F
    m = bits & ((1<<mbits)-1)
    if e==0:
        v = (m/(1<<mbits)) * 2**-14 if m else 0.0
    elif e==31:
        v = math.inf
    else:
        v = (1.0 + m/(1<<mbits)) * 2**(e-15)
    return sign*v

def decode_r11g11b10f(pix32):
    r = decode_minifloat(pix32 & 0x7FF, 5)
    g = decode_minifloat((pix32>>11) & 0x7FF, 5)
    b = decode_minifloat((pix32>>22) & 0x3FF, 4)
    return (r,g,b,1.0)

def pack_rgba16f(rgba):
    # struct 'e' = half float
    return struct.pack('<eeee', *rgba)

# ---- 读取全部 level 数据并转 RGBA16F ----
levels_data=[]
for l in range(levels):
    (face_size,)=struct.unpack_from('<I',d,off); off+=4
    w=max(1,pw>>l); h=max(1,ph>>l)
    # 逐 face 解码
    faces_frames=[]
    for f in range(faces):
        face_bytes=d[off:off+face_size]; off+=face_size
        # 每像素 4 字节（R11G11B10）
        out_faces=bytearray()
        for i in range(0, face_size, 4):
            pix=struct.unpack_from('<I', face_bytes, i)[0]
            out_faces += pack_rgba16f(decode_r11g11b10f(pix))
        faces_frames.append(bytes(out_faces))
    # KTX2 每 level = 6 face 连续
    levels_data.append((w,h,b''.join(faces_frames)))
    print(f'level {l}: {w}x{h} ktx2_face={len(faces_frames[0])} bytes')

# ---- 写 KTX2 ----
level_sizes=[len(dd[2]) for dd in levels_data]
header_size=72
level_index_size=levels*8
dfd_off=header_size+level_index_size
data_off=dfd_off+len(SKY_DFD)

# 头部 72 字节
hdr=bytearray()
hdr+=b'\xABKTX 20\xBB\r\n\x1A\n'   # magic
hdr+=struct.pack('<I',97)            # vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT
hdr+=struct.pack('<I',2)             # typeSize
hdr+=struct.pack('<3I', pw, ph, 0)   # pixelWidth,Height,Depth
hdr+=struct.pack('<I',0)             # layerCount
hdr+=struct.pack('<I',faces)         # faceCount
hdr+=struct.pack('<I',levels)        # levelCount
hdr+=struct.pack('<I',0)             # supercompressionScheme
hdr+=struct.pack('<4I', dfd_off, len(SKY_DFD), 0, 0)  # dfd off/len, kvd off/len
hdr+=struct.pack('<2I', 0, 0)        # sgd off/len
assert len(hdr)==72, len(hdr)
# level index
idx=b''.join(struct.pack('<Q',s) for s in level_sizes)
# 数据
blob=hdr+idx+SKY_DFD
for w,h,faces_blob in levels_data:
    blob+=faces_blob
open(out,'wb').write(blob)
print('written', out, len(blob), 'bytes')
print('level_sizes', level_sizes)
