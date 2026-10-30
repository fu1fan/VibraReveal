#include <metal_stdlib>
using namespace metal;

// All grids support odd sizes and 1-pixel axes. No threadgroup-size divisibility required.
int reflect101(int i, int n) {
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    i = ((i % period) + period) % period;
    return i < n ? i : period - i;
}
float at(device const float *a, int x, int y, uint w, uint h) {
    return a[reflect101(y, int(h)) * w + reflect101(x, int(w))];
}
float linear(device const float *a, float x, float y, uint w, uint h) {
    x = clamp(x, 0.0f, float(w - 1)); y = clamp(y, 0.0f, float(h - 1));
    uint x0 = uint(x), y0 = uint(y), x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
    return mix(mix(a[y0*w+x0], a[y0*w+x1], x-float(x0)),
               mix(a[y1*w+x0], a[y1*w+x1], x-float(x0)), y-float(y0));
}
float resized(device const float *a, uint2 id, uint w, uint h, uint sw, uint sh) {
    return linear(a, (float(id.x)+0.5f)*float(sw)/float(w)-0.5f,
                  (float(id.y)+0.5f)*float(sh)/float(h)-0.5f, sw, sh);
}
// Spatial box integration before decimation, including fractional edge coverage.
kernel void ingest(device const uchar *raw [[buffer(0)]], device float *out [[buffer(1)]],
                   constant uint *p [[buffer(2)]], uint2 id [[thread_position_in_grid]]) {
    uint w=p[0], h=p[1]; if (id.x>=w || id.y>=h) return;
    float x0=float(id.x)*float(p[6])/float(w), x1=float(id.x+1)*float(p[6])/float(w);
    float y0=float(id.y)*float(p[7])/float(h), y1=float(id.y+1)*float(p[7])/float(h);
    float sum=0;
    for (uint y=uint(floor(y0)); y<min(uint(ceil(y1)),p[7]); ++y) {
        float wy=min(y1,float(y+1))-max(y0,float(y));
        for (uint x=uint(floor(x0)); x<min(uint(ceil(x1)),p[6]); ++x) {
            float wx=min(x1,float(x+1))-max(x0,float(x));
            uint offset=(p[5]+y)*p[8]+(p[4]+x)*p[9];
            uint v=raw[offset]; if (p[9]==2) v |= uint(raw[offset+1])<<8;
            uint mask=(1u<<p[10])-1u;
            sum += float(v & mask)/float(mask)*wx*wy;
        }
    }
    out[id.y*w+id.x]=sum/((x1-x0)*(y1-y0));
}
kernel void blur(device const float *src [[buffer(0)]], device float *dst [[buffer(1)]],
                 constant uint *p [[buffer(2)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x>=p[0] || id.y>=p[1]) return;
    const float k[5]={1.f/16,4.f/16,6.f/16,4.f/16,1.f/16};
    float value=0;
    for(int i=-2;i<=2;++i) value+=k[i+2]*at(src,int(id.x)+(p[12]==0?i:0),int(id.y)+(p[12]==1?i:0),p[0],p[1]);
    dst[id.y*p[0]+id.x]=value;
}
kernel void shrink(device const float *src [[buffer(0)]], device float *dst [[buffer(1)]],
                   constant uint *p [[buffer(2)]], uint2 id [[thread_position_in_grid]]) {
    if(id.x>=p[0] || id.y>=p[1]) return;
    dst[id.y*p[0]+id.x]=resized(src,id,p[0],p[1],p[2],p[3]);
}
kernel void subtract(device const float *src [[buffer(0)]], device const float *small [[buffer(1)]],
                     device float *band [[buffer(2)]], constant uint *p [[buffer(3)]], uint2 id [[thread_position_in_grid]]) {
    if(id.x>=p[0] || id.y>=p[1]) return;
    band[id.y*p[0]+id.x]=src[id.y*p[0]+id.x]-resized(small,id,p[0],p[1],p[2],p[3]);
}
struct History { float origin, previous, correction, x1, x2, y1, y2; };
kernel void phase(device const float *band [[buffer(0)]], device float *out [[buffer(1)]],
                  device History *state [[buffer(2)]], constant uint *p [[buffer(3)]],
                  constant float *c [[buffer(4)]], uint2 id [[thread_position_in_grid]]) {
    uint w=p[0], h=p[1]; if(id.x>=w || id.y>=h) return;
    uint j=id.y*w+id.x;
    const float even[5]={1.f/16,4.f/16,6.f/16,4.f/16,1.f/16};
    const float odd[5]={-1.f/8,-2.f/8,0,2.f/8,1.f/8};
    float rx=0,ry=0;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        float v=at(band,int(id.x)+x,int(id.y)+y,w,h);
        rx+=v*odd[x+2]*even[y+2]; ry+=v*even[x+2]*odd[y+2];
    }
    float r=sqrt(rx*rx+ry*ry)+1e-6f;
    float phi=atan2(band[j],r);
    if(p[13] == 1) { out[j] = phi; return; }
    if(p[13] == 2) {
        float angle = c[5] * state[j].origin;
        out[j] = band[j]*cos(angle) + r*sin(angle);
        return;
    }
    History s;
    if(p[11]) { s={phi,phi,0,0,0,0,0}; } else { s=state[j]; }
    float delta=phi-s.previous;
    float wrapped=delta-2*M_PI_F*floor((delta+M_PI_F)/(2*M_PI_F));
    if(wrapped==-M_PI_F && delta>0) wrapped=M_PI_F;
    if(abs(delta)>=M_PI_F) s.correction+=wrapped-delta;
    float x=phi+s.correction-s.origin;
    float y=c[0]*x+c[1]*s.x1+c[2]*s.x2-c[3]*s.y1-c[4]*s.y2;
    float angle=c[5]*y;
    out[j]=band[j]*cos(angle)+r*sin(angle);
    s.previous=phi; s.x2=s.x1; s.x1=x; s.y2=s.y1; s.y1=y; state[j]=s;
}
kernel void reconstruct(device const float *band [[buffer(0)]], device const float *small [[buffer(1)]],
                        device float *out [[buffer(2)]], constant uint *p [[buffer(3)]], uint2 id [[thread_position_in_grid]]) {
    if(id.x>=p[0] || id.y>=p[1]) return;
    out[id.y*p[0]+id.x]=band[id.y*p[0]+id.x]+resized(small,id,p[0],p[1],p[2],p[3]);
}
