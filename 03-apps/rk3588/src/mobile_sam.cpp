#include "mobile_sam.hpp"
#include "vulkan_motion.hpp"
#include <rknn_api.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <dlfcn.h>
namespace vr {
namespace {
void check(int status,const char* operation) {
    if(status<0) throw MotionError("RKNN_ERROR",std::string(operation)+": "+std::to_string(status));
}
struct Runtime {
    void* handle=nullptr;
    decltype(&rknn_init) init=nullptr;
    decltype(&rknn_destroy) destroy=nullptr;
    decltype(&rknn_query) query=nullptr;
    decltype(&rknn_inputs_set) inputsSet=nullptr;
    decltype(&rknn_run) run=nullptr;
    decltype(&rknn_outputs_get) outputsGet=nullptr;
    decltype(&rknn_outputs_release) outputsRelease=nullptr;
    template<class T> void symbol(T& p,const char* name) {
        p=reinterpret_cast<T>(dlsym(handle,name));
        if(!p) throw MotionError("RKNN_UNAVAILABLE",std::string("Missing symbol ")+name);
    }
    Runtime() {
        handle=dlopen("librknnrt.so",RTLD_NOW|RTLD_LOCAL);
        if(!handle) throw MotionError("RKNN_UNAVAILABLE","Cannot load librknnrt.so");
        try {
            symbol(init,"rknn_init"); symbol(destroy,"rknn_destroy"); symbol(query,"rknn_query");
            symbol(inputsSet,"rknn_inputs_set"); symbol(run,"rknn_run");
            symbol(outputsGet,"rknn_outputs_get"); symbol(outputsRelease,"rknn_outputs_release");
        } catch(...) { dlclose(handle); throw; }
    }
    ~Runtime() { if(handle) dlclose(handle); }
};
struct Model {
    Runtime& rt;
    rknn_context ctx=0;
    std::vector<rknn_tensor_attr> inputs,outputs;
    Model(Runtime& runtime,const std::string& path):rt(runtime) {
        std::ifstream file(path,std::ios::binary|std::ios::ate); auto size=file.tellg();
        if(!file || size<=0 || size>512*1024*1024) throw MotionError("MODEL_ERROR","Invalid RKNN model file");
        std::vector<char> bytes(static_cast<size_t>(size)); file.seekg(0);
        if(!file.read(bytes.data(),size)) throw MotionError("MODEL_ERROR","Cannot read model");
        check(rt.init(&ctx,bytes.data(),uint32_t(size),0,nullptr),"init");
        try {
            rknn_input_output_num io{}; check(rt.query(ctx,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io)),"io counts");
            if(io.n_input>16 || io.n_output>16) throw MotionError("MODEL_ERROR","Unexpected IO count");
            inputs.resize(io.n_input); outputs.resize(io.n_output);
            for(uint32_t i=0;i<inputs.size();++i) { inputs[i].index=i; check(rt.query(ctx,RKNN_QUERY_INPUT_ATTR,&inputs[i],sizeof(inputs[i])),"input attrs"); }
            for(uint32_t i=0;i<outputs.size();++i) { outputs[i].index=i; check(rt.query(ctx,RKNN_QUERY_OUTPUT_ATTR,&outputs[i],sizeof(outputs[i])),"output attrs"); }
        } catch(...) { rt.destroy(ctx); ctx=0; throw; }
    }
    ~Model() { if(ctx) rt.destroy(ctx); }
    std::vector<std::vector<float>> infer(std::vector<rknn_input> input) {
        if(input.size()!=inputs.size()) throw MotionError("MODEL_ERROR","Input count mismatch");
        check(rt.inputsSet(ctx,uint32_t(input.size()),input.data()),"inputs_set"); check(rt.run(ctx,nullptr),"run");
        std::vector<rknn_output> out(outputs.size());
        for(uint32_t i=0;i<out.size();++i) { out[i].index=i; out[i].want_float=1; }
        check(rt.outputsGet(ctx,uint32_t(out.size()),out.data(),nullptr),"outputs_get");
        std::vector<std::vector<float>> result;
        try {
            for(size_t i=0;i<out.size();++i) {
                if(!out[i].buf || out[i].size!=outputs[i].n_elems*sizeof(float)) throw MotionError("MODEL_ERROR","Output buffer shape mismatch");
                auto p=static_cast<float*>(out[i].buf); result.emplace_back(p,p+outputs[i].n_elems);
                for(float v:result.back()) if(!std::isfinite(v)) throw MotionError("MODEL_ERROR","Nonfinite model output");
            }
        } catch(...) { rt.outputsRelease(ctx,uint32_t(out.size()),out.data()); throw; }
        check(rt.outputsRelease(ctx,uint32_t(out.size()),out.data()),"outputs_release"); return result;
    }
};
float sample(const float* image,uint32_t w,uint32_t h,float x,float y) {
    x=std::clamp(x,0.f,float(w-1)); y=std::clamp(y,0.f,float(h-1));
    uint32_t x0=uint32_t(x),y0=uint32_t(y),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
    float dx=x-x0,dy=y-y0;
    return (1-dy)*((1-dx)*image[y0*w+x0]+dx*image[y0*w+x1])+dy*((1-dx)*image[y1*w+x0]+dx*image[y1*w+x1]);
}
rknn_input tensor(uint32_t index,std::vector<float>& data,rknn_tensor_format format) {
    rknn_input input{}; input.index=index; input.buf=data.data(); input.size=uint32_t(data.size()*4);
    input.type=RKNN_TENSOR_FLOAT32; input.fmt=format; return input;
}
}
struct MobileSam::Impl {
    Runtime rt;
    Model encoder,decoder;
    Frame identity;
    uint32_t resizedW=0,resizedH=0,slots=0;
    bool ready=false;
    std::vector<float> embedding;
    Impl(const std::string& e,const std::string& d):encoder(rt,e),decoder(rt,d) {
        if(encoder.inputs.size()!=1 || encoder.outputs.size()!=1 || decoder.inputs.size()!=5 || decoder.outputs.size()!=2)
            throw MotionError("MODEL_ERROR","Require Rockchip MobileSAM encoder and five-input decoder");
        auto& ei=encoder.inputs[0]; auto& eo=encoder.outputs[0]; auto& di=decoder.inputs[0]; auto& masks=decoder.outputs[1];
        bool imageShape=ei.n_dims==4 && ei.dims[0]==1 &&
            ((ei.fmt==RKNN_TENSOR_NHWC && ei.dims[1]==448 && ei.dims[2]==448 && ei.dims[3]==3) ||
             (ei.fmt==RKNN_TENSOR_NCHW && ei.dims[1]==3 && ei.dims[2]==448 && ei.dims[3]==448));
        slots=decoder.inputs[2].n_elems;
        if(!imageShape || eo.fmt!=RKNN_TENSOR_NCHW || eo.n_dims!=4 || eo.dims[0]!=1 ||
           eo.n_elems!=di.n_elems || slots<2 || slots>64 || decoder.inputs[1].n_elems!=2*slots ||
           decoder.inputs[3].n_elems!=112*112 || decoder.inputs[4].n_elems!=1 ||
           masks.fmt!=RKNN_TENSOR_NCHW || masks.n_dims!=4 || masks.dims[0]!=1 || masks.dims[2]!=112 || masks.dims[3]!=112 ||
           masks.dims[1]!=decoder.outputs[0].n_elems)
            throw MotionError("MODEL_ERROR","Unsupported tensor layout; use documented 448/112 MobileSAM export");
    }
};
MobileSam::MobileSam(const std::string& e,const std::string& d):impl(new Impl(e,d)) {}
MobileSam::~MobileSam()=default;
uint32_t MobileSam::promptCapacity() const { return impl->slots; }
void MobileSam::setImage(const Frame& f) {
    auto& s=*impl; s.ready=false;
    uint32_t bpp=f.bits==8?1:2;
    if(!f.width || !f.height || uint64_t(f.width)*f.height>16*1024*1024 ||
       (f.bits!=8 && f.bits!=10 && f.bits!=12 && f.bits!=16) ||
       f.stride<uint64_t(f.width)*bpp || uint64_t(f.stride)*f.height>f.data.size())
        throw MotionError("INVALID_FRAME","Invalid grayscale selection frame");
    std::vector<float> gray(size_t(f.width)*f.height);
    uint32_t max=(1u<<f.bits)-1;
    for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
        size_t p=size_t(y)*f.stride+x*bpp; uint32_t v=f.data[p]; if(bpp==2) v|=uint32_t(f.data[p+1])<<8;
        gray[size_t(y)*f.width+x]=float(v&max)*255/max;
    }
    double scale=448.0/std::max(f.width,f.height);
    s.resizedW=std::max(1u,uint32_t(f.width*scale+.5)); s.resizedH=std::max(1u,uint32_t(f.height*scale+.5));
    std::vector<float> rgb(448*448*3,0);
    for(uint32_t y=0;y<s.resizedH;++y) for(uint32_t x=0;x<s.resizedW;++x) {
        float v=sample(gray.data(),f.width,f.height,(x+.5f)*f.width/s.resizedW-.5f,(y+.5f)*f.height/s.resizedH-.5f);
        for(uint32_t ch=0;ch<3;++ch) rgb[(y*448+x)*3+ch]=v;
    }
    // Channel mean/std are baked into RKNN conversion; input is RGB 0..255.
    auto result=s.encoder.infer({tensor(0,rgb,RKNN_TENSOR_NHWC)});
    auto& attr=s.encoder.outputs[0]; uint32_t channels=attr.dims[1],h=attr.dims[2],w=attr.dims[3];
    s.embedding.resize(result[0].size());
    for(uint32_t c=0;c<channels;++c) for(uint32_t j=0;j<h*w;++j) s.embedding[j*channels+c]=result[0][c*h*w+j];
    s.identity.width=f.width; s.identity.height=f.height; s.identity.sequence=f.sequence; s.identity.timestamp=f.timestamp;
    s.ready=true;
}
SelectionResult MobileSam::select(const std::vector<SelectionPrompt>& prompts) {
    auto& s=*impl; const auto& f=s.identity;
    if(!s.ready) throw MotionError("NO_IMAGE","Call setImage before selection");
    if(prompts.empty()) throw MotionError("INVALID_PROMPT","Require points or a box");
    unsigned first=0,last=0,positive=0;
    const SelectionPrompt *top=nullptr,*bottom=nullptr;
    for(const auto& p:prompts) {
        if(!std::isfinite(p.x) || !std::isfinite(p.y) || p.x<0 || p.y<0 || p.x>=f.width || p.y>=f.height || p.label<0 || p.label>3)
            throw MotionError("INVALID_PROMPT","Prompt outside image or invalid label");
        if(p.label==1) ++positive;
        if(p.label==2) { ++first; top=&p; } if(p.label==3) { ++last; bottom=&p; }
    }
    if(first!=last || first>1 || (!first && !positive) || (first && (top->x>=bottom->x || top->y>=bottom->y)))
        throw MotionError("INVALID_PROMPT","Require positive point or valid top-left/bottom-right box");
    if(prompts.size()+(!first?1:0)>s.slots) throw MotionError("PROMPT_CAPACITY","Too many prompts for decoder; point-only input needs one padding slot");
    std::vector<float> coords(2*s.slots,0),labels(s.slots,-1),mask(112*112,0),hasMask(1,0);
    for(size_t i=0;i<prompts.size();++i) {
        coords[2*i]=prompts[i].x*s.resizedW/f.width; coords[2*i+1]=prompts[i].y*s.resizedH/f.height; labels[i]=float(prompts[i].label);
    }
    auto result=s.decoder.infer({tensor(0,s.embedding,RKNN_TENSOR_NHWC),tensor(1,coords,RKNN_TENSOR_UNDEFINED),
        tensor(2,labels,RKNN_TENSOR_UNDEFINED),tensor(3,mask,RKNN_TENSOR_NHWC),tensor(4,hasMask,RKNN_TENSOR_UNDEFINED)});
    size_t best=std::max_element(result[0].begin(),result[0].end())-result[0].begin();
    auto logits=result[1].data()+best*112*112;
    // Match SAM: bilinear upscale to padded encoder size, crop, upscale to original.
    std::vector<float> cropped(s.resizedW*s.resizedH);
    for(uint32_t y=0;y<s.resizedH;++y) for(uint32_t x=0;x<s.resizedW;++x)
        cropped[y*s.resizedW+x]=sample(logits,112,112,(x+.5f)/4-.5f,(y+.5f)/4-.5f);
    SelectionResult out{f.width,f.height,f.sequence,f.timestamp,result[0][best],{}}; out.mask.resize(size_t(f.width)*f.height);
    for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x)
        out.mask[size_t(y)*f.width+x]=sample(cropped.data(),s.resizedW,s.resizedH,(x+.5f)*s.resizedW/f.width-.5f,(y+.5f)*s.resizedH/f.height-.5f)>0?255:0;
    return out;
}
}
