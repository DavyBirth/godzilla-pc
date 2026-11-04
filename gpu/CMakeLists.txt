# Native Windows adapter; reference sources remain unchanged in third_party.
# SPDX-License-Identifier: GPL-2.0-or-later
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SHAD "${PROJECT_SOURCE_DIR}/third_party/shadPS4")
include(dependencies.cmake)
set(GZ_IMGUI "${PROJECT_SOURCE_DIR}/third_party/bloodborne_pc/gpu/third_party/imgui")
add_library(gz_imgui STATIC "${GZ_IMGUI}/imgui.cpp" "${GZ_IMGUI}/imgui_draw.cpp" "${GZ_IMGUI}/imgui_tables.cpp" "${GZ_IMGUI}/imgui_widgets.cpp" "${GZ_IMGUI}/backends/imgui_impl_vulkan.cpp")
target_include_directories(gz_imgui PUBLIC "${GZ_IMGUI}" "${GZ_IMGUI}/backends")
target_compile_definitions(gz_imgui PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1)
target_link_libraries(gz_imgui PRIVATE Vulkan::Headers)
add_subdirectory("${SHAD}/src/video_core/host_shaders" "host_shaders")
file(GLOB_RECURSE VIDEO CONFIGURE_DEPENDS "${SHAD}/src/video_core/*.cpp")
list(FILTER VIDEO EXCLUDE REGEX "(renderdoc|vk_presenter)\\.cpp$")
# Kyoei and bbport identified a real inconsistency in the pinned GPU:
# MarkAsMaybeDirty hashes guest_size, while RefreshImage compared only 8x8 pixels.
# Compare the same complete range, including CPU writes after the first 64 pixels.
list(REMOVE_ITEM VIDEO "${SHAD}/src/video_core/texture_cache/texture_cache.cpp")
file(READ "${SHAD}/src/video_core/texture_cache/texture_cache.cpp" TEXTURE_SOURCE)
set(TEXTURE_PARTIAL_HASH [=[        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);]=])
string(FIND "${TEXTURE_SOURCE}" "${TEXTURE_PARTIAL_HASH}" TEXTURE_HASH_POSITION)
if(TEXTURE_HASH_POSITION EQUAL -1)
    message(FATAL_ERROR "Texture dirty hash changed; review full-range comparison")
endif()
string(REPLACE "${TEXTURE_PARTIAL_HASH}"
    "        // Match the complete range hashed by MarkAsMaybeDirty.\n        const u64 hash = XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);"
    TEXTURE_SOURCE "${TEXTURE_SOURCE}")
function(gz_texture_replace old new)
    string(FIND "${TEXTURE_SOURCE}" "${old}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Texture prompt upload path changed; review adapter")
    endif()
    string(REPLACE "${old}" "${new}" updated "${TEXTURE_SOURCE}")
    set(TEXTURE_SOURCE "${updated}" PARENT_SCOPE)
endfunction()
set(TEXTURE_SOURCE "#include \"${PROJECT_SOURCE_DIR}/gpu/pc_prompts.h\"\n#include \"${PROJECT_SOURCE_DIR}/gpu/pc_photo_radio_texture.h\"\n${TEXTURE_SOURCE}")
gz_texture_replace("void TextureCache::RefreshImage(Image& image) {"
    "void TextureCache::RefreshImage(Image& image) {\n    bool radio_changed=false,prompt_changed=false; unsigned prompt_mode=0;\n    const auto* prompt=!image.usage.render_target ? GzPhotoRadioTexture::Select(image,True(image.flags & ImageFlagBits::CpuDirty),radio_changed,prompt_mode) : nullptr;\n    const bool radio_override=prompt!=nullptr;\n    if(!prompt && !image.usage.render_target) prompt=GzPcPrompts::Select(image,True(image.flags & ImageFlagBits::CpuDirty),prompt_changed,prompt_mode);\n    if(prompt_changed || radio_changed) image.flags |= ImageFlagBits::CpuDirty;")
gz_texture_replace("    const auto [in_buffer, in_offset] =\n        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);"
    "    auto source=buffer_cache.ObtainBufferForImage(image.info.guest_address,image.info.guest_size);\n    if(prompt){\n        auto& staging=buffer_cache.GetUtilityBuffer(MemoryUsage::Upload);\n        const auto [pixels,at]=staging.Map(image.info.guest_size,16);\n        ASSERT(pixels);std::memset(pixels,0,image.info.guest_size);\n        if(!radio_override){\n            const auto& data=prompt->variant[prompt_mode];\n            const bool copied=GzPcPrompts::Rows(*prompt,image.info,[&](size_t to,size_t from,size_t bytes){std::memcpy(pixels+to,data.data()+from,bytes);return true;});\n            ASSERT(copied);\n        }\n        staging.Commit();source={&staging,at};\n    }\n    const auto [in_buffer,in_offset]=source;")
gz_texture_replace("tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);"
    "(prompt ? TileManager::Result{in_buffer->Handle(),in_offset} : tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info));")
gz_texture_replace("if (is_gpu_modified && !is_gpu_dirty) {" "if (is_gpu_modified && !is_gpu_dirty && !prompt && !radio_changed) {")
gz_texture_replace("void TextureCache::UnregisterImage(ImageId image_id) {\n    Image& image = slot_images[image_id];"
    "void TextureCache::UnregisterImage(ImageId image_id) {\n    Image& image = slot_images[image_id];\n    GzPcPrompts::states.erase(image.image_uid);\n    GzPhotoRadioTexture::states.erase(image.image_uid);")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/texture_cache.cpp" CONTENT "${TEXTURE_SOURCE}" @ONLY)
list(APPEND VIDEO "${CMAKE_CURRENT_BINARY_DIR}/texture_cache.cpp")
# The reference stores Profile as raw bytes, including compiler padding. Compare
# declared fields rather than padding, and reject truncated profile files.
list(REMOVE_ITEM VIDEO "${SHAD}/src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp")
file(READ "${SHAD}/src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp" SERIAL_SOURCE)
file(READ "${SHAD}/src/shader_recompiler/profile.h" PROFILE_HEADER)
string(REGEX MATCHALL "(u64|u32|bool) [a-zA-Z0-9_]+" PROFILE_FIELDS "${PROFILE_HEADER}")
set(PROFILE_EQUAL "true")
set(PROFILE_DIFFERENCES "")
foreach(PROFILE_FIELD IN LISTS PROFILE_FIELDS)
    string(REGEX REPLACE "^(u64|u32|bool) " "" PROFILE_FIELD "${PROFILE_FIELD}")
    string(APPEND PROFILE_EQUAL " && saved_profile.${PROFILE_FIELD} == profile.${PROFILE_FIELD}")
    string(APPEND PROFILE_DIFFERENCES "        if (profile_size_ok && saved_profile.${PROFILE_FIELD} != profile.${PROFILE_FIELD}) LOG_INFO(Render, \"Pipeline cache field ${PROFILE_FIELD}: saved={} current={}\", saved_profile.${PROFILE_FIELD}, profile.${PROFILE_FIELD});\n")
endforeach()
set(PROFILE_COMPARE_OLD "    if (std::memcmp(profile_data.data(), &profile, sizeof(profile)) != 0) {")
set(PROFILE_COMPARE_NEW "    Shader::Profile saved_profile{};\n    const bool profile_size_ok = profile_data.size() == sizeof(profile);\n    if (profile_size_ok) std::memcpy(&saved_profile, profile_data.data(), sizeof(profile));\n    const bool profile_fields_equal = profile_size_ok && (${PROFILE_EQUAL});\n    if (profile_fields_equal && std::memcmp(profile_data.data(), &profile, sizeof(profile)) != 0)\n        LOG_INFO(Render, \"Pipeline cache profile fields match; ignored compiler padding differences\");\n    if (!profile_fields_equal) {")
string(FIND "${SERIAL_SOURCE}" "${PROFILE_COMPARE_OLD}" PROFILE_COMPARE_POSITION)
if(PROFILE_COMPARE_POSITION EQUAL -1 OR NOT PROFILE_FIELDS)
    message(FATAL_ERROR "Pipeline profile comparison changed; review the Windows cache adapter")
endif()
string(REPLACE "${PROFILE_COMPARE_OLD}" "${PROFILE_COMPARE_NEW}" SERIAL_SOURCE "${SERIAL_SOURCE}")
string(REPLACE "    if (!profile_fields_equal) {" "    if (!profile_fields_equal) {\n${PROFILE_DIFFERENCES}" SERIAL_SOURCE "${SERIAL_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vk_pipeline_serialization.cpp" CONTENT "${SERIAL_SOURCE}" @ONLY)
list(APPEND VIDEO "${CMAKE_CURRENT_BINARY_DIR}/vk_pipeline_serialization.cpp")
file(GLOB_RECURSE SHADERS CONFIGURE_DEPENDS "${SHAD}/src/shader_recompiler/*.cpp")
file(GLOB AVPLAYER CONFIGURE_DEPENDS "${SHAD}/src/core/libraries/avplayer/*.cpp")
list(REMOVE_ITEM AVPLAYER "${SHAD}/src/core/libraries/avplayer/avplayer_state.cpp")
file(READ "${SHAD}/src/core/libraries/avplayer/avplayer_state.cpp" AV_STATE_SOURCE)
set(AV_STATE_STORE "    m_current_state.store(state);")
set(AV_STATE_CLEAR "    m_event_queue.Clear();")
string(FIND "${AV_STATE_SOURCE}" "${AV_STATE_STORE}" AV_STATE_STORE_AT)
string(FIND "${AV_STATE_SOURCE}" "${AV_STATE_CLEAR}" AV_STATE_CLEAR_AT)
if(AV_STATE_STORE_AT EQUAL -1 OR AV_STATE_CLEAR_AT EQUAL -1)
    message(FATAL_ERROR "AvPlayer lifecycle changed; review PC input context adapter")
endif()
string(REPLACE "${AV_STATE_STORE}" "${AV_STATE_STORE}\n    GzPcPlayback::SetActive(this, state == AvState::Play || state == AvState::Pause || state == AvState::Buffering || state == AvState::Starting || state == AvState::Jump || state == AvState::TrickMode);" AV_STATE_SOURCE "${AV_STATE_SOURCE}")
string(REPLACE "${AV_STATE_CLEAR}" "${AV_STATE_CLEAR}\n    GzPcPlayback::SetActive(this, false);" AV_STATE_SOURCE "${AV_STATE_SOURCE}")
string(PREPEND AV_STATE_SOURCE "#include \"pc_playback.h\"\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/avplayer_state.cpp" CONTENT "${AV_STATE_SOURCE}" @ONLY)
list(APPEND AVPLAYER "${CMAKE_CURRENT_BINARY_DIR}/avplayer_state.cpp")
file(GLOB AJM CONFIGURE_DEPENDS "${SHAD}/src/core/libraries/ajm/*.cpp")
list(REMOVE_ITEM AJM "${SHAD}/src/core/libraries/ajm/ajm_aac.cpp")
file(READ "${SHAD}/src/core/libraries/ajm/ajm_aac.cpp" AJM_AAC_SOURCE)
string(REPLACE "externals/aacdec/fdk-aac/" "${gz_aac_SOURCE_DIR}/" AJM_AAC_SOURCE "${AJM_AAC_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ajm_aac.cpp" CONTENT "${AJM_AAC_SOURCE}" @ONLY)
list(APPEND AJM "${CMAKE_CURRENT_BINARY_DIR}/ajm_aac.cpp")
# Decode into host staging memory, then publish through the physical alias.
# Writing the tracked guest pages row by row can invalidate textures while the
# chroma plane still contains zeros from memset (a transient green frame).
list(REMOVE_ITEM AVPLAYER "${SHAD}/src/core/libraries/avplayer/avplayer_source.cpp")
file(READ "${SHAD}/src/core/libraries/avplayer/avplayer_source.cpp" AV_SOURCE)
set(AV_COPY_ORIGINAL "    CopyNV12Data(p_buffer, frame, m_use_vdec2);")
set(AV_COPY_STAGED "    const auto copy_width = m_use_vdec2 ? u32(frame.width) : Common::AlignUp(u32(frame.width), 16);\n    const auto copy_height = m_use_vdec2 ? u32(frame.height) : Common::AlignUp(u32(frame.height), 16);\n    std::vector<u8> staged((copy_width * copy_height * 3) / 2);\n    CopyNV12Data(staged.data(), frame, m_use_vdec2);\n    if (!Core::Memory::Instance()->TryWriteBacking(p_buffer, staged.data(), staged.size())) {\n        std::memcpy(p_buffer, staged.data(), staged.size());\n    }")
string(FIND "${AV_SOURCE}" "${AV_COPY_ORIGINAL}" AV_COPY_POSITION)
if(AV_COPY_POSITION EQUAL -1)
    message(FATAL_ERROR "AvPlayer frame copy changed; review the Windows staging adapter")
endif()
string(REPLACE "${AV_COPY_ORIGINAL}" "${AV_COPY_STAGED}" AV_SOURCE "${AV_SOURCE}")
# A false GetVideoData result means the game keeps displaying its previous
# pointer. Do not let the decoder overwrite that buffer until a replacement
# frame is actually ready. The source guarantees at least two frame buffers.
set(AV_RECYCLE "    if (m_current_video_frame.has_value()) {\n        m_video_buffers.Push(std::move(m_current_video_frame->buffer));\n        m_current_video_frame.reset();\n        m_video_buffers_cv.Notify();\n    }\n")
string(FIND "${AV_SOURCE}" "${AV_RECYCLE}" AV_RECYCLE_POSITION)
if(AV_RECYCLE_POSITION EQUAL -1)
    message(FATAL_ERROR "AvPlayer buffer recycling changed; review frame lifetime adapter")
endif()
string(REPLACE "${AV_RECYCLE}" "" AV_SOURCE "${AV_SOURCE}")
# Godzilla binds the delivered NV12 address directly as a texture. Its submitted
# draws must finish before the decoder may overwrite the previous address.
# Host-only test buffers have no guest mapping and need no GPU synchronization.
string(REPLACE "    if (m_current_video_frame.has_value()) {" "    if (m_current_video_frame.has_value()) {\n        const auto* gpu_wait = std::getenv(\"GZ_VIDEO_GPU_WAIT\");\n        if ((!gpu_wait || gpu_wait[0] != '0') && Core::Memory::Instance()->ClampRangeSize(reinterpret_cast<VAddr>(m_current_video_frame->info.p_data), 1))\n            Core::Memory::Instance()->WaitForGpuIdle();" AV_RECYCLE "${AV_RECYCLE}")
string(REPLACE "    auto frame = m_video_frames.Pop();" "${AV_RECYCLE}\n    auto frame = m_video_frames.Pop();" AV_SOURCE "${AV_SOURCE}")
# Diagnose video pacing without logging every poll that repeats the current frame.
set(AV_FRAME_POP "    auto frame = m_video_frames.Pop();\n    video_info = frame->info;")
set(AV_FRAME_TRACE "    auto frame = m_video_frames.Pop();\n    static thread_local std::chrono::steady_clock::time_point last_delivery{};\n    static thread_local u64 last_timestamp = 0, delivered = 0;\n    const auto delivery_now = std::chrono::steady_clock::now();\n    const auto delivery_gap = last_delivery.time_since_epoch().count() ? std::chrono::duration_cast<std::chrono::milliseconds>(delivery_now - last_delivery).count() : 0;\n    if (++delivered % 60 == 0 || delivery_gap > 100) {\n        LOG_INFO(Lib_AvPlayer, \"Video pacing: frame={} timestamp={} previous={} gap_ms={} queued={}\", delivered, frame->info.timestamp, last_timestamp, delivery_gap, m_video_frames.Size());\n    }\n    last_delivery = delivery_now; last_timestamp = frame->info.timestamp;\n    video_info = frame->info;")
string(FIND "${AV_SOURCE}" "${AV_FRAME_POP}" AV_POP_POSITION)
if(AV_POP_POSITION EQUAL -1)
    message(FATAL_ERROR "AvPlayer delivery changed; review video pacing diagnostics")
endif()
string(REPLACE "${AV_FRAME_POP}" "${AV_FRAME_TRACE}" AV_SOURCE "${AV_SOURCE}")
# Opt-in sparse luma measurements distinguish decoded fades from rendering flicker.
set(AV_LUMA_TRACE [=[    if (std::getenv("GZ_VIDEO_TRACE")) {
        const auto* luma = reinterpret_cast<const u8*>(frame->info.p_data);
        const u32 width = frame->info.details.video.width;
        const u32 height = frame->info.details.video.height;
        const u32 pitch = m_use_vdec2 ? width : Common::AlignUp(width, 16);
        u64 total = 0, count = 0;
        for (u32 y = 0; y < height; y += 16)
            for (u32 x = 0; x < width; x += 16) { total += luma[u64(y) * pitch + x]; ++count; }
        LOG_INFO(Lib_AvPlayer, "Video luma: timestamp={} mean={} buffer={}",
                 frame->info.timestamp, count ? double(total) / count : 0.0,
                 reinterpret_cast<uintptr_t>(frame->info.p_data));
    }
    video_info = frame->info;]=])
string(REPLACE "    video_info = frame->info;" "${AV_LUMA_TRACE}" AV_SOURCE "${AV_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/avplayer_source.cpp" CONTENT "${AV_SOURCE}" @ONLY)
list(APPEND AVPLAYER "${CMAKE_CURRENT_BINARY_DIR}/avplayer_source.cpp")
set_source_files_properties(${AVPLAYER} PROPERTIES COMPILE_DEFINITIONS GZ_NATIVE_AVPLAYER_THREADS=1)
set_source_files_properties("${SHAD}/src/core/libraries/audio/sdl_audio_out.cpp" PROPERTIES COMPILE_OPTIONS -msse4.1)
set(COMMON decoder error io_file native_clock number_utils rdtsc spin_lock recursive_lock string_util signal_context)
list(TRANSFORM COMMON PREPEND "${SHAD}/src/common/")
list(TRANSFORM COMMON APPEND ".cpp")
file(READ "${SHAD}/src/common/thread.cpp" THREAD_SOURCE)
string(REPLACE "#include \"ntapi.h\"" "#include \"common/ntapi.h\"" THREAD_SOURCE "${THREAD_SOURCE}")
string(REPLACE "#include \"core/libraries/kernel/threads/pthread.h\"" "" THREAD_SOURCE "${THREAD_SOURCE}")
string(REPLACE "    if (Libraries::Kernel::g_curthread) {\n        Libraries::Kernel::g_curthread->name = name;\n    }\n" "" THREAD_SOURCE "${THREAD_SOURCE}")
string(REPLACE "    using namespace Libraries::Kernel;\n    if (g_curthread && !g_curthread->name.empty()) {\n        return g_curthread->name;\n    }\n" "" THREAD_SOURCE "${THREAD_SOURCE}")
string(REPLACE "    PWSTR name;\n    GetThreadDescription(GetCurrentThread(), &name);\n    return Common::UTF16ToUTF8(name);" "    PWSTR name = nullptr;\n    if (FAILED(GetThreadDescription(GetCurrentThread(), &name))) return {};\n    auto result = Common::UTF16ToUTF8(name);\n    LocalFree(name);\n    return result;" THREAD_SOURCE "${THREAD_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/thread.cpp" CONTENT "${THREAD_SOURCE}" @ONLY)
# Registration can run without opening a window for --list-stubs.
file(READ "${SHAD}/src/core/libraries/gnmdriver/gnmdriver.cpp" GNM_SOURCE)
string(REPLACE "liverpool = std::make_unique<AmdGpu::Liverpool>();" "if (g_window) liverpool = std::make_unique<AmdGpu::Liverpool>();" GNM_SOURCE "${GNM_SOURCE}")
string(REPLACE "presenter = std::make_unique<Vulkan::Presenter>(*g_window, liverpool.get());" "if (g_window) presenter = std::make_unique<Vulkan::Presenter>(*g_window, liverpool.get());" GNM_SOURCE "${GNM_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gnmdriver.cpp" CONTENT "${GNM_SOURCE}" @ONLY)
file(READ "${SHAD}/src/core/libraries/videoout/video_out.cpp" VO_SOURCE)
string(REPLACE "    driver = std::make_unique<VideoOutDriver>" "    if (presenter) driver = std::make_unique<VideoOutDriver>" VO_SOURCE "${VO_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/video_out.cpp" CONTENT "${VO_SOURCE}" @ONLY)
file(READ "${SHAD}/src/core/libraries/videoout/driver.cpp" VO_DRIVER_SOURCE)
# Blank frames are recorded on the presentation thread. Sharing draw_scheduler
# here races GPU Finish/recording; bbport likewise uses the present scheduler.
set(VO_BLANK_OLD "const auto empty_frame = presenter->PrepareBlankFrame(false);")
string(FIND "${VO_DRIVER_SOURCE}" "${VO_BLANK_OLD}" VO_BLANK_POSITION)
if(VO_BLANK_POSITION EQUAL -1)
    message(FATAL_ERROR "VideoOut blank-frame path changed; review scheduler ownership")
endif()
string(REPLACE "${VO_BLANK_OLD}" "const auto empty_frame = presenter->PrepareBlankFrame(true);" VO_DRIVER_SOURCE "${VO_DRIVER_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/video_out_driver.cpp" CONTENT "${VO_DRIVER_SOURCE}" @ONLY)
file(READ "${SHAD}/src/core/libraries/audio/audioout.cpp" AUDIO_SOURCE)
string(REPLACE "if (EmulatorSettings.GetAudioBackend() == AudioBackend::OpenAL) {\n        audio = std::make_unique<OpenALAudioOut>();\n    } else {\n        audio = std::make_unique<SDLAudioOut>();\n    }" "audio = std::make_unique<SDLAudioOut>();" AUDIO_SOURCE "${AUDIO_SOURCE}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/audioout.cpp" CONTENT "${AUDIO_SOURCE}" @ONLY)
add_library(gzgpu STATIC ${VIDEO} ${SHADERS} ${COMMON} ${AVPLAYER} ${AJM}
    bridge.cpp window.cpp pad.cpp logging.cpp paths.cpp self_test.cpp ajm_test.cpp windows_api.cpp "${CMAKE_CURRENT_BINARY_DIR}/thread.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/audioout.cpp" "${SHAD}/src/core/libraries/audio/sdl_audio_out.cpp"
    vendor/vk_presenter.cpp vendor/equeue.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/gnmdriver.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/video_out_driver.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/video_out.cpp")
add_dependencies(gzgpu host_shaders)
target_sources(gzgpu PRIVATE "${PROJECT_SOURCE_DIR}/src/native_hook.cpp")
target_sources(gzgpu PRIVATE "${PROJECT_SOURCE_DIR}/src/camera_capture.cpp")
target_sources(gzgpu PRIVATE "${PROJECT_SOURCE_DIR}/src/pc_photo_native_scene.cpp")
target_sources(gzgpu PRIVATE "${PROJECT_SOURCE_DIR}/src/pc_pause_menu.cpp")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/photo.vert" PHOTO_VERTEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/photo.frag" PHOTO_FRAGMENT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS shaders/photo.vert shaders/photo.frag)
configure_file(shaders/photo_shaders.h.in "${CMAKE_CURRENT_BINARY_DIR}/photo_shaders.h" @ONLY)
set_source_files_properties(pc_photo_post.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(gzgpu PRIVATE pc_overlay.cpp pc_photo_post.cpp pc_photo_present.cpp "${PROJECT_SOURCE_DIR}/src/pc_features.cpp" "${PROJECT_SOURCE_DIR}/src/pc_photo.cpp" "${PROJECT_SOURCE_DIR}/src/pc_photo_presets.cpp" "${PROJECT_SOURCE_DIR}/src/pc_controller.cpp")
set_source_files_properties(pc_overlay.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1)
target_link_libraries(gzgpu PUBLIC gz_imgui gdiplus gdi32 user32)
target_include_directories(gzgpu PRIVATE shim "${SHAD}/src" "${SHAD}" "${HOST_SHADERS_INCLUDE}"
    "${SHAD}/externals/gcn/include" "${PROJECT_SOURCE_DIR}/third_party/bloodborne_pc/gpu/third_party/half/include"
    "${PROJECT_SOURCE_DIR}/src" "${SHAD}/src/core/libraries/gnmdriver"
    "${SHAD}/src/core/libraries" "${SHAD}/src/core/libraries/ajm"
    "${gz_mp3_SOURCE_DIR}")
target_compile_definitions(gzgpu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    BOOST_ASIO_DISABLE_CO_AWAIT VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
target_compile_options(gzgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers -fno-omit-frame-pointer)
target_link_libraries(gzgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator SDL3::SDL3-static xxhash miniz
    Zydis glslang::glslang glslang::SPIRV sirit xbyak::xbyak ffmpeg onecore gz_atrac9_codec fdk-aac)
