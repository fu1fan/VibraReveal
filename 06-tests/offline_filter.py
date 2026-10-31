#!/usr/bin/env python3
"""Whole-clip reference and cross-platform fixture.
 prepare DIR: generate padded 16-bit VRI1 fixture and independent NumPy outputs.
 check DIR VRV: validate Vulkan against NumPy and zero-phase interior response.
 compare DIR VRV VRM: additionally compare native macOS Metal CLI output.
"""
import json
from pathlib import Path
import struct
import sys
import zlib
import numpy as np


def resize(a, shape):
    h, w = shape; sh, sw = a.shape
    x = np.clip((np.arange(w)+.5)*sw/w-.5, 0, sw-1)
    y = np.clip((np.arange(h)+.5)*sh/h-.5, 0, sh-1)
    x0, y0 = x.astype(int), y.astype(int)
    x1, y1 = np.minimum(x0+1,sw-1), np.minimum(y0+1,sh-1)
    dx, dy = x-x0, (y-y0)[:,None]
    return ((1-dy)*((1-dx)*a[y0[:,None],x0]+dx*a[y0[:,None],x1])
            +dy*((1-dx)*a[y1[:,None],x0]+dx*a[y1[:,None],x1]))


def conv(a, weights, axis):
    pads=[(0,0),(0,0)]; pads[axis]=(2,2)
    padded=np.pad(a,pads,mode='reflect' if a.shape[axis]>1 else 'edge')
    return sum(v*np.take(padded,range(i,i+a.shape[axis]),axis=axis) for i,v in enumerate(weights))


def spatial(a):
    bands=[]; quadratures=[]; phases=[]
    even=np.array([1,4,6,4,1])/16
    odd=np.array([-1,-2,0,2,1])/8
    for _ in range(3):
        h,w=a.shape
        if h==w==1: break
        small=resize(conv(conv(a,even,1),even,0),(max(1,h//2),max(1,w//2)))
        band=a-resize(small,a.shape)
        rx=conv(conv(band,odd,1),even,0); ry=conv(conv(band,even,1),odd,0)
        r=np.hypot(rx,ry)+1e-6
        bands.append(band);quadratures.append(r);phases.append(np.arctan2(band,r).ravel())
        a=small
    return bands,quadratures,np.concatenate(phases).astype('float32'),a


def coefficients():
    f0=np.sqrt(5/120)*np.sqrt(15/120)
    q=max(.5,f0/((15-5)/120));omega=2*np.pi*f0;a=np.sin(omega)/(2*q)
    return np.array([a/(1+a),0,-a/(1+a),-2*np.cos(omega)/(1+a),(1-a)/(1+a)],dtype='float32').astype('float64')


def recurrence(values):
    c=coefficients();x1=values[0].astype('float64');x2=x1.copy();y1=np.zeros_like(x1);y2=y1.copy();result=[]
    for sample in values:
        x=sample.astype('float64');y=c[0]*x+c[1]*x1+c[2]*x2-c[3]*y1-c[4]*y2
        result.append(y.astype('float32'));x2,x1,y2,y1=x1,x,y1,y
    return np.array(result)


def prepare(directory):
    directory.mkdir(parents=True,exist_ok=True)
    count,w,h,stride=480,21,17,46
    y,x=np.mgrid[:h,:w];stack=[]
    def header(kind,seq,ts,size=0,crc=0):
        return struct.pack('<4sHHIIIIIIQQIIQIIII',b'VRI1',1,kind,2,size,w,h,stride,16,seq,ts,120,1,0,crc,w,h,0)
    with (directory/'moving.vri').open('wb') as f:
        f.write(header(1,0,0))
        for n in range(count):
            shift=.02*np.sin(2*np.pi*10*n/120)
            a=((.5+.3*np.sin((x+shift)*.5)*np.cos(y*.23))*65535).astype('<u2')
            padded=np.full((h,stride//2),65535,dtype='<u2');padded[:,:w]=a;data=padded.tobytes()
            f.write(header(2,n,round(n*1e9/120),len(data),zlib.crc32(data)));f.write(data)
            stack.append(a.astype('float64')/65535)
        f.write(header(3,count,count*h*stride))
    stack=np.array(stack); np.save(directory/'input.npy',stack)
    phases=np.array([spatial(a)[2] for a in stack])
    padding=min(count-1,int(np.ceil(3*120/5)))
    extended=np.concatenate([2*phases[:1]-phases[padding:0:-1],phases,2*phases[-1:]-phases[-2:-padding-2:-1]])
    filtered=recurrence(recurrence(extended)[::-1])[::-1][padding:padding+count]
    causal=recurrence(phases)
    for name,phase_stack in [('zero-phase',filtered),('causal',causal)]:
        outputs=[]
        for original,flat in zip(stack,phase_stack):
            bands,rs,_,small=spatial(original);amplified=[];offset=0
            for band,r in zip(bands,rs):
                signal=flat[offset:offset+band.size].reshape(band.shape);offset+=band.size
                amplified.append(band*np.cos(20*signal)+r*np.sin(20*signal))
            for band in reversed(amplified):small=band+resize(small,band.shape)
            outputs.append(small)
        np.save(directory/(name+'.npy'),np.array(outputs))
    print('Prepared 480-frame 16-bit padded fixture; padding=',padding)


def read_vrv(path):
    frames=[]
    with Path(path).open('rb') as f:
        for n in range(480):
            header=f.read(40);magic,w,h,size,seq,ts,num,den=struct.unpack('<4sIIIQQII',header)
            assert (magic,w,h,size,seq,ts,num,den)==(b'VRV1',21,17,21*17*4,n,round(n*1e9/120),120,1)
            frames.append(np.frombuffer(f.read(size),'<f4').reshape(h,w))
        assert not f.read(1)
    return np.array(frames)


def read_vrm(path):
    frames=[]
    with Path(path).open('rb') as f:
        for n in range(480):
            length=struct.unpack('<I',f.read(4))[0];meta=json.loads(f.read(length))
            assert meta['sequence']==n and meta['width']==21 and meta['height']==17
            frames.append(np.frombuffer(f.read(21*17*4),'<f4').reshape(17,21))
        assert not f.read(1)
    return np.array(frames)


if __name__=='__main__':
    mode=sys.argv[1];directory=Path(sys.argv[2])
    if mode=='prepare':prepare(directory)
    else:
        actual=read_vrv(sys.argv[3]);expected=np.load(directory/'zero-phase.npy')
        error=float(np.max(np.abs(actual-expected)));assert error<2e-4,error
        original=np.load(directory/'input.npy');n=np.arange(120,360)
        added=actual[120:360,5,7]-original[120:360,5,7]
        sine=np.dot(added,np.sin(2*np.pi*10*n/120));cosine=np.dot(added,np.cos(2*np.pi*10*n/120))
        ratio=abs(cosine)/max(abs(sine),1e-12)
        assert np.dot(added,added)>1e-6 and ratio<.03,ratio
        print('PASS: NumPy max error=',error,'interior quadrature/in-phase=',ratio)
        if mode=='compare':
            metal=read_vrm(sys.argv[4]);difference=float(np.max(np.abs(actual-metal)));assert difference<2e-4,difference
            print('PASS: native Metal vs Vulkan maximum error=',difference)
