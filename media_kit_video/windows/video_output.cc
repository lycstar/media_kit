// This file is a part of media_kit
// (https://github.com/alexmercerind/media_kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "video_output.h"

#include <algorithm>

// Only used for fallback software rendering, when hardware does not support
// DirectX 11 i.e. enough to support ANGLE. There is no way I'm allowing
// rendering anything higher 1080p with CPU on some retarded old computer. The
// thing might blow up in flames.
// TODO(@alexmercerind): Now that multiple flutter::TextureVariant (& respective
// FlutterDesktopGpuSurfaceDescriptor or FlutterDesktopPixelBuffer) are kept
// alive and disposed safely after usage, it's no longer necessary to
// pre-allocate a fixed size buffer for software rendering. However, this
// refactor is very-low priority for now. On the other hand, limiting to 1080p
// for S/W rendering seems actually good.
#define SW_RENDERING_MAX_WIDTH 1920
#define SW_RENDERING_MAX_HEIGHT 1080
#define SW_RENDERING_PIXEL_BUFFER_SIZE \
  SW_RENDERING_MAX_WIDTH* SW_RENDERING_MAX_HEIGHT * 4

VideoOutput::VideoOutput(int64_t handle,
                         std::optional<int64_t> width,
                         std::optional<int64_t> height,
                         flutter::PluginRegistrarWindows* registrar,
                         ThreadPool* thread_pool_ref)
    : handle_(reinterpret_cast<mpv_handle*>(handle)),
      width_(width),
      height_(height),
      registrar_(registrar),
      thread_pool_ref_(thread_pool_ref) {
  // The constructor must be invoked through the thread pool, because
  // |ANGLESurfaceManager| & libmpv render context creation can conflict with
  // the existing |Render| or |Resize| calls from another |VideoOutput|
  // instances (which will result in access violation).
  auto future = thread_pool_ref_->Post([&]() {
    // First try to initialize video playback with hardware acceleration &
    // |ANGLESurfaceManager|, use S/W API as fallback.
    auto is_hardware_acceleration_enabled = false;
    // Attempt to use H/W rendering.
    try {
      mpv_set_option_string(handle_, "video-sync", "audio");
      mpv_set_option_string(handle_, "video-timing-offset", "0");
      // OpenGL context needs to be set before |mpv_render_context_create|.
      surface_manager_ = std::make_unique<ANGLESurfaceManager>(
          static_cast<int32_t>(width_.value_or(1)),
          static_cast<int32_t>(height_.value_or(1)));
      Resize(width_.value_or(1), height_.value_or(1));
      mpv_opengl_init_params gl_init_params{
          [](auto, auto name) {
            return reinterpret_cast<void*>(eglGetProcAddress(name));
          },
          nullptr,
      };
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
          {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      // Request H/W decoding.
      mpv_set_option_string(handle_, "hwdec", "auto");
      // Create render context.
      if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
        mpv_render_context_set_update_callback(
            render_context_,
            [](void* context) {
              // Notify Flutter that a new frame is available. The actual
              // rendering will take place in the |Render| method, which will be
              // called by Flutter on the render thread.
              auto that = reinterpret_cast<VideoOutput*>(context);
              that->NotifyRender();
            },
            reinterpret_cast<void*>(this));
        // Set flag to true, indicating that H/W rendering is supported.
        is_hardware_acceleration_enabled = true;
        std::cout << "media_kit: VideoOutput: Using H/W rendering."
                  << std::endl;
      }
    } catch (...) {
      // Do nothing.
      // Likely received an |std::runtime_error| from |ANGLESurfaceManager|,
      // which indicates that H/W rendering is not supported.
    }
    if (!is_hardware_acceleration_enabled) {
      std::cout << "media_kit: VideoOutput: Using S/W rendering." << std::endl;
      // Allocate a "large enough" buffer ahead of time.
      pixel_buffer_ =
          std::make_unique<uint8_t[]>(SW_RENDERING_PIXEL_BUFFER_SIZE);
      Resize(width_.value_or(1), height_.value_or(1));
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
        mpv_render_context_set_update_callback(
            render_context_,
            [](void* context) {
              // Notify Flutter that a new frame is available. The actual
              // rendering will take place in the |Render| method, which will be
              // called by Flutter on the render thread.
              auto that = reinterpret_cast<VideoOutput*>(context);
              that->NotifyRender();
            },
            reinterpret_cast<void*>(this));
      }
    }
  });
  future.wait();
}

VideoOutput::~VideoOutput() {
  destroyed_ = true;
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(texture_id_);
  }
  // Add one more task into the thread pool queue & exit the destructor only
  // when it gets executed. This will ensure that all the tasks posted to
  // the thread pool i.e. render or resize before this are executed (and won't
  // reference the dead object anymore), most notably |CheckAndResize| &
  // |Render|.
  auto future = thread_pool_ref_->Post([&, id = texture_id_]() {
    if (id) {
      std::cout << "media_kit: VideoOutput: Free Texture: " << id << std::endl;
      if (texture_variants_.find(id) != texture_variants_.end()) {
        texture_variants_.erase(id);
      }
      // H/W
      if (textures_.find(id) != textures_.end()) {
        textures_.erase(id);
      }
      // S/W
      if (pixel_buffer_textures_.find(id) != pixel_buffer_textures_.end()) {
        pixel_buffer_textures_.erase(id);
      }
    }
    std::cout << "VideoOutput::~VideoOutput: "
              << reinterpret_cast<int64_t>(handle_) << std::endl;
    // Free (call destructor) |ANGLESurfaceManager| through the thread pool.
    // This will ensure synchronized EGL or ANGLE usage & won't conflict with
    // |Render| or |CheckAndResize| of other |VideoOutput|s.
    surface_manager_.reset(nullptr);
  });
  future.wait();
  if (render_context_) {
    mpv_render_context_free(render_context_);
  }
}

void VideoOutput::NotifyRender() {
  if (destroyed_) {
    return;
  }
  thread_pool_ref_->Post(std::bind(&VideoOutput::CheckAndResize, this));
  thread_pool_ref_->Post(std::bind(&VideoOutput::Render, this));
}

void VideoOutput::Render() {
  if (texture_id_) {
    // H/W
    if (surface_manager_ != nullptr) {
      surface_manager_->Draw([&]() {
        mpv_opengl_fbo fbo{
            0,
            surface_manager_->width(),
            surface_manager_->height(),
            0,
        };
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(render_context_, params);
      });
    }
    // S/W
    if (pixel_buffer_ != nullptr) {
      int32_t size[]{
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->width),
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->height),
      };
      auto pitch = 4 * size[0];
      mpv_render_param params[]{
          {MPV_RENDER_PARAM_SW_SIZE, size},
          {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
          {MPV_RENDER_PARAM_SW_POINTER, pixel_buffer_.get()},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      mpv_render_context_render(render_context_, params);
    }
    try {
      // Notify Flutter that a new frame is available.
      registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id_);
    } catch (...) {
      // Prevent any redundant exceptions if the texture is unregistered etc.
    }
  }
}

void VideoOutput::SetTextureUpdateCallback(
    std::function<void(int64_t, int64_t, int64_t)> callback) {
  texture_update_callback_ = callback;
  texture_update_callback_(texture_id_, GetVideoWidth(), GetVideoHeight());
}

void VideoOutput::CheckAndResize() {
  // Check if a new texture with different dimensions is needed.
  auto required_width = GetVideoWidth(), required_height = GetVideoHeight();
  if (required_width < 1 || required_height < 1) {
    // Invalid.
    return;
  }
  int64_t current_width = -1, current_height = -1;
  if (surface_manager_ != nullptr) {
    current_width = surface_manager_->width();
    current_height = surface_manager_->height();
  }
  if (pixel_buffer_ != nullptr) {
    current_width = pixel_buffer_textures_.at(texture_id_)->width;
    current_height = pixel_buffer_textures_.at(texture_id_)->height;
  }
  // Currently rendered video output dimensions.
  // Either H/W or S/W rendered.
  assert(current_width > 0);
  assert(current_height > 0);
  if (required_width == current_width && required_height == current_height) {
    // No creation of new texture required.
    return;
  }
  Resize(required_width, required_height);
}

void VideoOutput::Resize(int64_t required_width, int64_t required_height) {
  std::cout << required_width << " " << required_height << std::endl;
  // Unregister previously registered texture.
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(texture_id_);
    texture_id_ = 0;
    // Add one more task into the thread pool queue for clearing the previous
    // texture objects. This will ensure that all the tasks posted to the thread
    // pool before this are executed (and won't reference the dead object
    // anymore), most notably |CheckAndResize| & |Render|.
    thread_pool_ref_->Post([&, id = texture_id_]() {
      if (id) {
        std::cout << "media_kit: VideoOutput: Free Texture: " << id
                  << std::endl;
        if (texture_variants_.find(id) != texture_variants_.end()) {
          texture_variants_.erase(id);
        }
        // H/W
        if (textures_.find(id) != textures_.end()) {
          textures_.erase(id);
        }
        // S/W
        if (pixel_buffer_textures_.find(id) != pixel_buffer_textures_.end()) {
          pixel_buffer_textures_.erase(id);
        }
      }
    });
  }
  // H/W
  if (surface_manager_ != nullptr) {
    // Destroy internal ID3D11Texture2D & EGLSurface & create new with updated
    // dimensions while preserving previous EGLDisplay & EGLContext.
    surface_manager_->HandleResize(static_cast<int32_t>(required_width),
                                   static_cast<int32_t>(required_height));
    auto texture = std::make_unique<FlutterDesktopGpuSurfaceDescriptor>();
    texture->struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    texture->handle = surface_manager_->handle();
    texture->width = texture->visible_width = surface_manager_->width();
    texture->height = texture->visible_height = surface_manager_->height();
    texture->release_context = nullptr;
    texture->release_callback = [](void*) {};
    texture->format = kFlutterDesktopPixelFormatBGRA8888;
    auto texture_variant =
        std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [&](auto, auto) -> FlutterDesktopGpuSurfaceDescriptor* {
              if (texture_id_) {
                surface_manager_->Read();
                return textures_.at(texture_id_).get();
              }
              return nullptr;
            }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    textures_.emplace(std::make_pair(texture_id_, std::move(texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
  }
  // S/W
  if (pixel_buffer_ != nullptr) {
    auto pixel_buffer_texture = std::make_unique<FlutterDesktopPixelBuffer>();
    pixel_buffer_texture->buffer = pixel_buffer_.get();
    pixel_buffer_texture->width = required_width;
    pixel_buffer_texture->height = required_height;
    pixel_buffer_texture->release_context = nullptr;
    pixel_buffer_texture->release_callback = [](void*) {};
    auto texture_variant =
        std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [&](auto, auto) -> FlutterDesktopPixelBuffer* {
              if (texture_id_) {
                return pixel_buffer_textures_.at(texture_id_).get();
              }
              return nullptr;
            }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    pixel_buffer_textures_.emplace(
        std::make_pair(texture_id_, std::move(pixel_buffer_texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
  }
}

int64_t VideoOutput::GetVideoWidth() {
  // Fixed width.
  if (width_) {
    return width_.value();
  }
  // Video resolution dependent width.
  int64_t width = 0;
  mpv_get_property(handle_, "width", MPV_FORMAT_INT64, &width);
  if (pixel_buffer_ != nullptr) {
    // Limit width if software rendering is being used.
    return std::clamp(width, static_cast<int64_t>(0),
                      static_cast<int64_t>(SW_RENDERING_MAX_WIDTH));
  }
  return width;
}

int64_t VideoOutput::GetVideoHeight() {
  // Fixed height.
  if (height_) {
    return height_.value();
  }
  // Video resolution dependent height.
  int64_t height = 0;
  mpv_get_property(handle_, "height", MPV_FORMAT_INT64, &height);
  if (pixel_buffer_ != nullptr) {
    // Limit width if software rendering is being used.
    return std::clamp(height, static_cast<int64_t>(0),
                      static_cast<int64_t>(SW_RENDERING_MAX_HEIGHT));
  }
  return height;
}
