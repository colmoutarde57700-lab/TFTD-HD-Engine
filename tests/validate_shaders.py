from pathlib import Path
import re,ast,ctypes,json
import argparse
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[1];outdir=args.output;outdir.mkdir(parents=True,exist_ok=True)
s=(r/'src/Engine/HdGpuBackend.cpp').read_text();a=s.index('static const char spriteShader[] =');b=s.index('static const char ',a+20)
literals=lambda v: ''.join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:\\.|[^"\\])*"',v))
shader=''.join(literals(l) for l in s[a:b].splitlines() if l.lstrip().startswith('"'))
a=s.index('std::string spriteSource =');b=s.index('const std::string marker',a);prefix=literals(s[a:b]);hdr=(r/'src/Engine/HdCausticShader.h').read_text();shared=hdr.split('R"HLSL(',1)[1].split(')HLSL"',1)[0]
# C++ puts the functions between the cbuffer declaration and the roof helper.
pos=prefix.index('float3 roofCaustic');code=(prefix[:pos]+shared+prefix[pos:]+shader).replace('__HD_SCALE__','16');(outdir/'shader_test.hlsl').write_text(code)
dll=ctypes.WinDLL('d3dcompiler_47.dll');fn=dll.D3DCompile;fn.restype=ctypes.c_long;fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_void_p)]
def blobdata(p):
 v=ctypes.cast(p,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
 ptr=ctypes.WINFUNCTYPE(ctypes.c_void_p,ctypes.c_void_p)(v[3])(p);size=ctypes.WINFUNCTYPE(ctypes.c_size_t,ctypes.c_void_p)(v[4])(p);return ctypes.string_at(ptr,size)
def release(p):
 if p:
  v=ctypes.cast(p,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents;ctypes.WINFUNCTYPE(ctypes.c_ulong,ctypes.c_void_p)(v[2])(p)
a=s.index('static const char bedrockShader[] =');b=s.index('// The HLSL string',a)
bedrock=shared+''.join(literals(l) for l in s[a:b].splitlines() if l.lstrip().startswith(chr(34)))
results=[];data=code.encode()
for entry in ['VS','PS_RGBA','PS_ENV','PS_INDEXED','PS_BEDROCK','PS_SMOKE','PS_LOCAL_LIGHT','PS_PRESENT_RGBA','PS_PRESENT_INDEXED']:
 data=(bedrock if entry=='PS_BEDROCK' else code).encode();out=ctypes.c_void_p();err=ctypes.c_void_p();hr=fn(data,len(data),b'P2ZJ_shaders',None,None,entry.encode(),b'vs_4_0' if entry=='VS' else b'ps_4_0',1<<11,0,ctypes.byref(out),ctypes.byref(err));message=blobdata(err).decode(errors='replace') if err else '';size=len(blobdata(out)) if out else 0;results.append(dict(entry=entry,hresult=hr,bytes=size,diagnostics=message));print(entry,hr,size,message[:200]);release(out);release(err)
(outdir/'shader_validation.json').write_text(json.dumps(results,indent=2));assert all(x['hresult']>=0 for x in results)
