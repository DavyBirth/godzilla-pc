// Host photographic grading; no guest image writes or ordinary-frame readback.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_photo_post.h"
#include "photo_shaders.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <array>
#include <stdexcept>
#include <vector>

namespace GzPhoto {
namespace {
template <class T> T checked(vk::ResultValue<T> result) {
    if (result.result != vk::Result::eSuccess)
        throw std::runtime_error("Photo preview Vulkan allocation failed");
    return std::move(result.value);
}
bool supported(vk::Format f) {
    return f == vk::Format::eB8G8R8A8Srgb || f == vk::Format::eR8G8B8A8Srgb ||
           f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eR8G8B8A8Unorm;
}
} // namespace
struct PostProcessor::State {
    vk::Device device;
    vk::Format format;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniqueDescriptorPool pool;
    vk::UniqueSampler sampler;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    std::vector<vk::DescriptorSet> sets;
    State(const Vulkan::Instance &instance, vk::Format value, unsigned slots)
        : device(instance.GetDevice()), format(value) {
        if (!supported(format) || !slots || slots > 32)
            throw std::invalid_argument("Photo preview format or slot count unsupported");
        const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler,
                                                     1, vk::ShaderStageFlagBits::eFragment};
        descriptors = checked(
            device.createDescriptorSetLayoutUnique({.bindingCount = 1, .pBindings = &binding}));
        const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, slots};
        pool = checked(device.createDescriptorPoolUnique(
            {.maxSets = slots, .poolSizeCount = 1, .pPoolSizes = &size}));
        sampler = checked(
            device.createSamplerUnique({.magFilter = vk::Filter::eLinear,
                                        .minFilter = vk::Filter::eLinear,
                                        .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                        .maxLod = 0}));
        const auto descriptor_layout = *descriptors;
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(GradePush)};
        layout = checked(device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                            .pSetLayouts = &descriptor_layout,
                                                            .pushConstantRangeCount = 1,
                                                            .pPushConstantRanges = &push}));
        vk::UniqueShaderModule vs{
            Vulkan::Compile(VertexSource, vk::ShaderStageFlagBits::eVertex, device), device};
        vk::UniqueShaderModule fs{
            Vulkan::Compile(FragmentSource, vk::ShaderStageFlagBits::eFragment, device), device};
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
        const vk::PipelineVertexInputStateCreateInfo input{};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{
            .topology = vk::PrimitiveTopology::eTriangleList};
        const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo raster{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .lineWidth = 1};
        const vk::PipelineMultisampleStateCreateInfo samples{.rasterizationSamples =
                                                                 vk::SampleCountFlagBits::e1};
        const vk::PipelineColorBlendAttachmentState color{
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1,
                                                          .pAttachments = &color};
        const std::array dynamics{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic{
            .dynamicStateCount = unsigned(dynamics.size()), .pDynamicStates = dynamics.data()};
        const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                        .pColorAttachmentFormats = &format};
        pipeline = checked(device.createGraphicsPipelineUnique(
            {}, vk::GraphicsPipelineCreateInfo{.pNext = &rendering,
                                               .stageCount = unsigned(stages.size()),
                                               .pStages = stages.data(),
                                               .pVertexInputState = &input,
                                               .pInputAssemblyState = &assembly,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &samples,
                                               .pColorBlendState = &blend,
                                               .pDynamicState = &dynamic,
                                               .layout = *layout}));
        std::vector<vk::DescriptorSetLayout> layouts(slots, descriptor_layout);
        sets = checked(device.allocateDescriptorSets(
            {.descriptorPool = *pool, .descriptorSetCount = slots, .pSetLayouts = layouts.data()}));
    }
};
PostProcessor::PostProcessor(const Vulkan::Instance &instance, vk::Format format, unsigned slots)
    : state(std::make_unique<State>(instance, format, slots)) {}
PostProcessor::~PostProcessor() = default;
bool PostProcessor::Render(vk::CommandBuffer cmd, unsigned slot, vk::ImageView source,
                           unsigned width, unsigned height, vk::ImageView target,
                           vk::Extent2D extent, Grade grade) {
    if (!Valid(grade) || Neutral(grade) || !width || !height || !extent.width || !extent.height ||
        !source || !target || source == target || slot >= state->sets.size())
        return false;
    const auto viewport = FitPhotoViewport(width, height, extent.width, extent.height);
    if (!viewport.width || !viewport.height)
        return false;
    const auto set = state->sets[slot];
    const vk::DescriptorImageInfo image{*state->sampler, source, vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSet write{.dstSet = set,
                                       .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                       .pImageInfo = &image};
    state->device.updateDescriptorSets(write, {});
    const vk::RenderingAttachmentInfo attachment{.imageView = target,
                                                 .imageLayout =
                                                     vk::ImageLayout::eColorAttachmentOptimal,
                                                 .loadOp = vk::AttachmentLoadOp::eLoad,
                                                 .storeOp = vk::AttachmentStoreOp::eStore};
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &attachment});
    cmd.setViewport(0, vk::Viewport{float(viewport.x), float(viewport.y), float(viewport.width),
                                    float(viewport.height), 0, 1});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *state->pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *state->layout, 0, set, {});
    const bool srgb =
        state->format == vk::Format::eB8G8R8A8Srgb || state->format == vk::Format::eR8G8B8A8Srgb;
    const GradePush push{grade, srgb ? 0 : 1, width, height};
    cmd.pushConstants(*state->layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    return true;
}
} // namespace GzPhoto
