#include "vulkan_motion.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace vr {
using Clock=std::chrono::steady_clock;
static void check(VkResult result, const char* operation) {
    if(result!=VK_SUCCESS) throw MotionError("VULKAN_ERROR",std::string(operation)+": VkResult="+std::to_string(result));
}
void MotionConfig::validate() const {
    if(!width || !height || uint64_t(width)*height>1024*1024 || levels<1 || levels>8 ||
       !std::isfinite(fps) || fps<1 || fps>1000 || !std::isfinite(low) || !std::isfinite(high) ||
       !(0<low && low<high && high<fps/2) || !std::isfinite(alpha) || alpha<0 || alpha>200)
        throw MotionError("INVALID_CONFIG","Require ROI 1..1048576 pixels, levels 1..8, fps 1..1000, 0<low<high<fps/2, alpha 0..200");
}
std::array<float,5> MotionConfig::coefficients() const {
    const double f0=std::sqrt(low/fps)*std::sqrt(high/fps);
    const double q=std::max(0.5,f0/((high-low)/fps));
    const double omega=2*std::acos(-1)*f0,a=std::sin(omega)/(2*q);
    return {float(a/(1+a)),0,float(-a/(1+a)),float(-2*std::cos(omega)/(1+a)),float((1-a)/(1+a))};
}
void EdgeConfig::validate() const {
    if(!std::isfinite(threshold) || threshold<=0 || threshold>2 || boundaryRadius>8 || regionRadius>16)
        throw MotionError("INVALID_CONFIG","Edge threshold must be (0,2], boundary radius <=8, region radius <=16");
}
struct VulkanMotion::Impl {
    MotionConfig config;
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout=VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkDescriptorSet descriptor=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkCommandPool commandPool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    float* mapped=nullptr;
    std::string name;
    bool fresh=true, failed=false;
    uint64_t lastSequence=0,lastTimestamp=0;
    uint32_t sourceWidth=0,sourceHeight=0,sourceBits=0,sourceStride=0;
    struct Level { uint32_t w,h,sw,sh,tmp,blur,small,band,amplified,history,reconstructed; };
    std::vector<Level> levels;
    uint32_t analysisBase=0;
    struct Params {
        uint32_t op,w,h,sw,sh,src,other,dst,history,fresh;
        float b0,b2,a1,a2,alpha;
    };
    static_assert(sizeof(Params)==60,"GLSL push-constant layout");
    Params parameters{};
    ~Impl() {
        // Never release resources referenced by pending work, including after a
        // fence timeout. The CLI reports errors before this cleanup wait.
        if(device) vkDeviceWaitIdle(device);
        if(mapped) vkUnmapMemory(device,memory);
        if(fence) vkDestroyFence(device,fence,nullptr);
        if(commandPool) vkDestroyCommandPool(device,commandPool,nullptr);
        if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if(shader) vkDestroyShaderModule(device,shader,nullptr);
        if(pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        if(descriptorPool) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
        if(descriptorLayout) vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
        if(buffer) vkDestroyBuffer(device,buffer,nullptr);
        if(memory) vkFreeMemory(device,memory,nullptr);
        if(device) vkDestroyDevice(device,nullptr);
        if(instance) vkDestroyInstance(instance,nullptr);
    }
    void barrier(VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask=srcAccess; b.dstAccessMask=dstAccess;
        vkCmdPipelineBarrier(command,srcStage,dstStage,0,1,&b,0,nullptr,0,nullptr);
    }
    void dispatch(uint32_t op,uint32_t w,uint32_t h,uint32_t sw,uint32_t sh,
                  uint32_t src,uint32_t other,uint32_t dst,uint32_t history=0) {
        auto p=parameters;
        p.op=op; p.w=w; p.h=h; p.sw=sw; p.sh=sh; p.src=src; p.other=other;
        p.dst=dst; p.history=history; p.fresh=fresh;
        vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(p),&p);
        vkCmdDispatch(command,(w+7)/8,(h+7)/8,1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    }
    void init(const std::string& shaderPath) {
        config.validate();
        const auto c=config.coefficients();
        parameters.b0=c[0]; parameters.b2=c[2]; parameters.a1=c[3]; parameters.a2=c[4];
        parameters.alpha=config.alpha;
        if(!(parameters.b0>0 && std::abs(parameters.a2)<1 &&
             1.0+parameters.a1+parameters.a2>0 && 1.0-parameters.a1+parameters.a2>0))
            throw MotionError("INVALID_CONFIG","Temporal filter unstable at float32 precision");
        uint32_t count=config.width*config.height,w=config.width,h=config.height;
        auto alloc=[&](uint32_t n) { uint32_t offset=count; count+=n; return offset; };
        if(config.analysisOnly) analysisBase=alloc(10*w*h);
        for(uint32_t i=0;!config.analysisOnly && i<config.levels && (w>1 || h>1);++i) {
            uint32_t sw=std::max(1u,w/2),sh=std::max(1u,h/2),n=w*h;
            levels.push_back({w,h,sw,sh,alloc(n),alloc(n),alloc(sw*sh),alloc(n),alloc(n),alloc(7*n),alloc(n)});
            w=sw; h=sh;
        }
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        check(vkCreateInstance(&ici,nullptr,&instance),"vkCreateInstance");
        uint32_t n=0; check(vkEnumeratePhysicalDevices(instance,&n,nullptr),"enumerate devices");
        std::vector<VkPhysicalDevice> devices(n);
        check(vkEnumeratePhysicalDevices(instance,&n,devices.data()),"enumerate devices");
        VkPhysicalDevice physical=VK_NULL_HANDLE; uint32_t family=0;
        VkPhysicalDeviceProperties properties{};
        for(auto candidate:devices) {
            vkGetPhysicalDeviceProperties(candidate,&properties);
            uint32_t nf=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&nf,nullptr);
            std::vector<VkQueueFamilyProperties> families(nf);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&nf,families.data());
            for(uint32_t i=0;i<nf;++i) if(families[i].queueCount && (families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
                physical=candidate; family=i; break;
            }
            if(physical) break;
        }
        if(!physical) throw MotionError("VULKAN_UNAVAILABLE","No Vulkan compute device");
        name=properties.deviceName;
        VkDeviceSize bytes=VkDeviceSize(count)*sizeof(float);
        if(bytes>properties.limits.maxStorageBufferRange || sizeof(Params)>properties.limits.maxPushConstantsSize ||
           (config.width+7)/8>properties.limits.maxComputeWorkGroupCount[0] ||
           (config.height+7)/8>properties.limits.maxComputeWorkGroupCount[1])
            throw MotionError("RESOURCE_LIMIT","ROI exceeds Vulkan device limits");
        float priority=1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        check(vkCreateDevice(physical,&dci,nullptr,&device),"vkCreateDevice"); vkGetDeviceQueue(device,family,0,&queue);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=bytes; bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device,&bci,nullptr,&buffer),"vkCreateBuffer");
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req);
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
        uint32_t type=mp.memoryTypeCount;
        for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) &&
            (mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
        if(type==mp.memoryTypeCount) throw MotionError("RESOURCE_LIMIT","Host-coherent Vulkan storage memory required");
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=req.size; mai.memoryTypeIndex=type;
        check(vkAllocateMemory(device,&mai,nullptr,&memory),"vkAllocateMemory");
        check(vkBindBufferMemory(device,buffer,memory,0),"vkBindBufferMemory");
        void* address=nullptr; check(vkMapMemory(device,memory,0,bytes,0,&address),"vkMapMemory"); mapped=static_cast<float*>(address);
        std::memset(mapped,0,size_t(bytes));
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dlci.bindingCount=1; dlci.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&dlci,nullptr,&descriptorLayout),"descriptor layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&poolSize;
        check(vkCreateDescriptorPool(device,&dpci,nullptr,&descriptorPool),"descriptor pool");
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai.descriptorPool=descriptorPool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&descriptorLayout;
        check(vkAllocateDescriptorSets(device,&dsai,&descriptor),"descriptor set");
        VkDescriptorBufferInfo dbi{buffer,0,bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=descriptor; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&dbi;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount=1; plci.pSetLayouts=&descriptorLayout; plci.pushConstantRangeCount=1; plci.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device,&plci,nullptr,&pipelineLayout),"pipeline layout");
        std::ifstream file(shaderPath,std::ios::binary|std::ios::ate);
        auto size=file.tellg();
        if(!file || size<=0 || size%4 || size>16*1024*1024) throw MotionError("SHADER_ERROR","Invalid SPIR-V file: "+shaderPath);
        std::vector<uint32_t> code(size_t(size)/4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()),size);
        if(!file) throw MotionError("SHADER_ERROR","Cannot read SPIR-V");
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smci.codeSize=size_t(size); smci.pCode=code.data();
        check(vkCreateShaderModule(device,&smci,nullptr,&shader),"shader module");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpci.layout=pipelineLayout;
        cpci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module=shader; cpci.stage.pName="main";
        check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline),"compute pipeline");
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=family; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&cpi,nullptr,&commandPool),"command pool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=commandPool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&cai,&command),"command buffer");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fci,nullptr,&fence),"fence");
    }
    EdgeResult analyze(const Frame& f,const EdgeConfig& e,const std::vector<uint8_t>& mask) {
        if(failed) throw MotionError("SESSION_FAILED","Recreate processor after an error");
        if(!config.analysisOnly) throw MotionError("INVALID_CONFIG","Use analysisOnly processor for edges");
        e.validate();
        const auto& c=config;
        const uint32_t n=c.width*c.height,b=analysisBase,bpp=f.bits==8?1:2;
        if((f.bits!=8 && f.bits!=10 && f.bits!=12 && f.bits!=16) ||
           c.x>f.width || c.y>f.height || c.width>f.width-c.x || c.height>f.height-c.y ||
           uint64_t(f.stride)<uint64_t(f.width)*bpp || uint64_t(f.stride)*f.height>f.data.size() ||
           (!mask.empty() && mask.size()!=n))
            throw MotionError("INVALID_FRAME","Invalid frame or selection mask dimensions");
        const uint32_t maximum=(1u<<f.bits)-1;
        for(uint32_t y=0;y<c.height;++y) for(uint32_t x=0;x<c.width;++x) {
            size_t j=y*c.width+x, offset=size_t(y+c.y)*f.stride+size_t(x+c.x)*bpp;
            uint32_t v=f.data[offset]; if(bpp==2) v|=uint32_t(f.data[offset+1])<<8;
            mapped[j]=float(v&maximum)/maximum;
            mapped[b+2*n+j]=mask.empty() || mask[j]?1.f:0.f;
        }
        check(vkResetCommandBuffer(command,0),"reset analysis command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&begin),"begin analysis");
        barrier(VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptor,0,nullptr);
        dispatch(0,c.width,c.height,0,0,0,0,b);
        dispatch(1,c.width,c.height,0,0,b,0,b+n);
        // gradient planes: magnitude, normalX, normalY
        dispatch(6,c.width,c.height,0,0,b+n,0,b+3*n);
        parameters.alpha=e.threshold;
        dispatch(7,c.width,c.height,e.boundaryRadius,0,b+3*n,b+2*n,b+6*n);
        dispatch(8,c.width,c.height,e.regionRadius,0,b+7*n,0,b+8*n);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(command),"end analysis");
        check(vkResetFences(device,1,&fence),"reset analysis fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        check(vkQueueSubmit(queue,1,&submit,fence),"submit analysis");
        check(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ULL),"wait analysis");
        EdgeResult r; r.width=c.width; r.height=c.height; r.sequence=f.sequence; r.timestamp=f.timestamp;
        r.magnitude.assign(mapped+b+3*n,mapped+b+4*n);
        r.normalX.assign(mapped+b+4*n,mapped+b+5*n);
        r.normalY.assign(mapped+b+5*n,mapped+b+6*n);
        r.peaks.resize(n); r.edges.resize(n); r.regions.resize(n);
        uint64_t selected=0;
        for(uint32_t j=0;j<n;++j) {
            r.peaks[j]=mapped[b+6*n+j]>0?255:0;
            r.edges[j]=mapped[b+7*n+j]>0?255:0;
            r.regions[j]=mapped[b+8*n+j]>0?255:0;
            if(mapped[b+2*n+j]>0) { r.focusScore+=double(r.magnitude[j])*r.magnitude[j]; ++selected; }
        }
        if(selected) r.focusScore/=selected;
        return r;
    }
    size_t phaseCount() const {
        size_t count=0; for(const auto& l:levels) count+=size_t(l.w)*l.h; return count;
    }
    std::vector<float> process(const Frame& frame,uint32_t phaseMode=0,const std::vector<float>* phases=nullptr) {
        if(failed) throw MotionError("SESSION_FAILED","Recreate processor after an error");
        if(config.analysisOnly) throw MotionError("INVALID_CONFIG","Analysis processor cannot process motion");
        if(phaseMode && config.realtime) throw MotionError("INVALID_CONFIG","Phase extraction/reconstruction requires offline configuration");
        if(phaseMode==2 && (!phases || phases->size()!=phaseCount())) throw MotionError("INVALID_PHASES","Phase count mismatch");
        if(phases) for(float value:*phases) if(!std::isfinite(value)) throw MotionError("INVALID_PHASES","Nonfinite phase input");
        auto start=Clock::now();
        const auto& c=config;
        uint32_t bpp=frame.bits==8?1:2;
        if((frame.bits!=8 && frame.bits!=10 && frame.bits!=12 && frame.bits!=16) ||
           c.x>frame.width || c.y>frame.height || c.width>frame.width-c.x || c.height>frame.height-c.y ||
           uint64_t(frame.stride)<uint64_t(frame.width)*bpp || uint64_t(frame.stride)*frame.height>frame.data.size())
            throw MotionError("INVALID_FRAME","Invalid grayscale frame or ROI out of bounds");
        if(!fresh && phaseMode!=2) {
            if(frame.width!=sourceWidth || frame.height!=sourceHeight || frame.bits!=sourceBits || frame.stride!=sourceStride)
                throw MotionError("FORMAT_CHANGED","Capture format changed during session");
            if(lastSequence==UINT64_MAX || frame.sequence!=lastSequence+1)
                throw MotionError("FRAME_DROPPED","Nonconsecutive processing frame sequence");
            if(frame.timestamp<=lastTimestamp || std::abs(double(frame.timestamp-lastTimestamp)*c.fps/1e9-1)>0.2)
                throw MotionError("TIMESTAMP_GAP","Frame interval differs from configured FPS by more than 20%");
        }
        uint32_t mask=(1u<<frame.bits)-1;
        for(uint32_t y=0;y<c.height;++y) for(uint32_t x=0;x<c.width;++x) {
            size_t offset=size_t(y+c.y)*frame.stride+size_t(x+c.x)*bpp;
            uint32_t v=frame.data[offset]; if(bpp==2) v|=uint32_t(frame.data[offset+1])<<8;
            mapped[size_t(y)*c.width+x]=float(v&mask)/float(mask);
        }
        if(phaseMode==2) {
            size_t offset=0;
            for(const auto& l:levels) {
                size_t n=size_t(l.w)*l.h;
                std::copy_n(phases->data()+offset,n,mapped+l.history); offset+=n;
            }
        }
        check(vkResetCommandBuffer(command,0),"reset command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&begin),"begin command");
        barrier(VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptor,0,nullptr);
        uint32_t current=0;
        for(const auto& l:levels) {
            dispatch(0,l.w,l.h,0,0,current,0,l.tmp);
            dispatch(1,l.w,l.h,0,0,l.tmp,0,l.blur);
            dispatch(2,l.sw,l.sh,l.w,l.h,l.blur,0,l.small);
            dispatch(3,l.w,l.h,l.sw,l.sh,current,l.small,l.band);
            dispatch(phaseMode==1?9:phaseMode==2?10:4,l.w,l.h,0,0,l.band,0,l.amplified,l.history);
            current=l.small;
        }
        for(auto i=levels.rbegin();phaseMode!=1 && i!=levels.rend();++i) {
            const auto& l=*i;
            dispatch(5,l.w,l.h,l.sw,l.sh,l.amplified,current,l.reconstructed);
            current=l.reconstructed;
        }
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(command),"end command");
        check(vkResetFences(device,1,&fence),"reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        auto gpuStart=Clock::now();
        check(vkQueueSubmit(queue,1,&submit,fence),"queue submit");
        VkResult result=vkWaitForFences(device,1,&fence,VK_TRUE,c.realtime?uint64_t(1e9/c.fps):UINT64_MAX);
        double gpuMs=std::chrono::duration<double,std::milli>(Clock::now()-gpuStart).count();
        if(c.realtime && (result==VK_TIMEOUT || (result==VK_SUCCESS && gpuMs>1000/c.fps)))
            throw MotionError("GPU_DEADLINE_EXCEEDED","Vulkan submission exceeded frame budget; wait_ms="+std::to_string(gpuMs)+", budget_ms="+std::to_string(1000/c.fps)+"; reduce ROI or FPS");
        check(result,"wait fence");
        std::vector<float> output;
        if(phaseMode==1) {
            output.reserve(phaseCount());
            for(const auto& l:levels) output.insert(output.end(),mapped+l.amplified,mapped+l.amplified+size_t(l.w)*l.h);
        } else output.assign(mapped+current,mapped+current+size_t(c.width)*c.height);
        double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        if(c.realtime && elapsed>1/c.fps) throw MotionError("PROCESSING_DEADLINE_EXCEEDED","ROI upload + Vulkan + readback exceeded frame budget; seconds="+std::to_string(elapsed));
        fresh=false; lastSequence=frame.sequence; lastTimestamp=frame.timestamp;
        sourceWidth=frame.width; sourceHeight=frame.height; sourceBits=frame.bits; sourceStride=frame.stride;
        return output;
    }
};
VulkanMotion::VulkanMotion(const MotionConfig& config,const std::string& path):impl(new Impl) { impl->config=config; impl->init(path); }
VulkanMotion::~VulkanMotion()=default;
size_t VulkanMotion::phaseCount() const { return impl->phaseCount(); }
std::vector<float> VulkanMotion::extractPhases(const Frame& frame) {
    try { return impl->process(frame,1); } catch(...) { impl->failed=true; throw; }
}
std::vector<float> VulkanMotion::reconstruct(const Frame& frame,const std::vector<float>& phases) {
    try { return impl->process(frame,2,&phases); } catch(...) { impl->failed=true; throw; }
}
EdgeResult VulkanMotion::analyze(const Frame& f,const EdgeConfig& e,const std::vector<uint8_t>& mask) {
    try { return impl->analyze(f,e,mask); } catch(...) { impl->failed=true; throw; }
}
const std::string& VulkanMotion::deviceName() const { return impl->name; }
std::vector<float> VulkanMotion::process(const Frame& f) {
    try { return impl->process(f); } catch(...) { impl->failed=true; throw; }
}
}
