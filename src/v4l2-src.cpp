#include "v4l2/v4l2-src.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#include <fmt/core.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#pragma GCC diagnostic pop
#include <memory>
#include <span>

#include <atomic>
#include <csignal>

[[nodiscard]] constexpr GstClockTime ns_per_frame(FPSEnum fps)
{
    return 1'000'000'000 / static_cast<GstClockTime>(fps);
}

// Pad template: MJPEG over NVMM or raw YUY2
static GstStaticPadTemplate pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS(
                                "image/jpeg, "
                                "memory=(string)NVMM, "
                                "width=(int)[1,MAX], "
                                "height=(int)[1,MAX], "
                                "framerate=(fraction)[0/1,MAX]; "
                                "video/x-raw, "
                                "format=(string)YUY2, "
                                "width=(int)[1,MAX], "
                                "height=(int)[1,MAX], "
                                "framerate=(fraction)[0/1,MAX]"));

// Forward-declarations of GObject methods
template <typename T>
static T *get_instance(GObject *obj)
{
    return reinterpret_cast<T *>(GST_V4L2SRC(obj));
}

static void _v4l2src_set_property(GObject *object, guint prop_id,
                                      const GValue *value, GParamSpec *pspec);
static void _v4l2src_get_property(GObject *object, guint prop_id,
                                      GValue *value, GParamSpec *pspec);

// Lifecycle and pushsrc methods
static gboolean _v4l2src_start(GstBaseSrc *src);
static gboolean _v4l2src_stop(GstBaseSrc *src);
static GstCaps *_v4l2src_get_caps([[maybe_unused]] GstBaseSrc *basesrc, GstCaps *filter);
static GstFlowReturn _v4l2src_create(GstPushSrc *src, GstBuffer **buf);
static void _v4l2src_finalize(GObject *object);
static GstCaps *get_active_caps(const V4L2Src *self);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvolatile"
G_DEFINE_TYPE(V4L2Src, _v4l2src, GST_TYPE_PUSH_SRC)
#pragma GCC diagnostic pop

static gboolean _v4l2src_negotiate(GstBaseSrc *src)
{
    GstPad *srcpad = GST_BASE_SRC_PAD(src);
    GstCaps *our_caps = gst_pad_get_current_caps(srcpad);
    if (!our_caps)
    {
        our_caps = get_active_caps(get_instance<V4L2Src>(G_OBJECT(src)));
        if (!our_caps)
        {
            GST_ERROR_OBJECT(src, "failed to get active caps — you're flying blind");
            return FALSE;
        }
    }

    // optional: intersect with peer caps if available
    GstCaps *their_caps = gst_pad_peer_query_caps(srcpad, nullptr);
    GstCaps *negotiated_caps = nullptr;

    if (their_caps)
    {
        negotiated_caps = gst_caps_intersect_full(their_caps, our_caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(their_caps);
        gst_caps_unref(our_caps);
    }
    else
    {
        negotiated_caps = our_caps; // no peer? no problem. trust ourselves.
    }

    if (!negotiated_caps || !gst_caps_is_fixed(negotiated_caps))
    {
        GST_ERROR_OBJECT(src, "caps are not fixed after negotiation — upstream brought chaos");
        if (negotiated_caps)
            gst_caps_unref(negotiated_caps);
        return FALSE;
    }

    if (!gst_base_src_set_caps(src, negotiated_caps))
    {
        GST_ERROR_OBJECT(src, "gst_base_src_set_caps() failed — go cry to your pipeline");
        gst_caps_unref(negotiated_caps);
        return FALSE;
    }

    gst_caps_unref(negotiated_caps);
    return TRUE;
}

static GstCaps *_v4l2src_fixate(GstBaseSrc *src, GstCaps *caps)
{
    return GST_BASE_SRC_CLASS(_v4l2src_parent_class)->fixate(src, caps);
}

static void _v4l2src_class_init(V4L2SrcClass *klass)
{
    auto *gclass = G_OBJECT_CLASS(klass);
    gclass->set_property = _v4l2src_set_property;
    gclass->get_property = _v4l2src_get_property;
    gclass->finalize = _v4l2src_finalize;

    // Element metadata and pad template
    auto *eclass = GST_ELEMENT_CLASS(klass);

    // 1 = device - path
    g_object_class_install_property(
        gclass,
        1,
        g_param_spec_string(
            "device",
            "Device Path",
            "Path to the V4L2 device (e.g., /dev/video0)",
            DEFAULT_DEVICE_PATH,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    // 2 = pixel-format
    static const GEnumValue pixel_format_values[] = {
        {static_cast<int>(PixelFormatEnum::MJPG), "MJPG", "MJPG"},
        {static_cast<int>(PixelFormatEnum::YUYV), "YUYV", "YUYV"},
        {0, nullptr, nullptr}};
    const GType pixel_format_type = g_enum_register_static("PixelFormatEnum", pixel_format_values);
    g_object_class_install_property(
        gclass,
        2,
        g_param_spec_enum("pixel-format", "Pixel Format", "MJPG or YUYV", pixel_format_type,
                          static_cast<int>(DEFAULT_PIXEL_FORMAT),
                          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_assert(G_IS_PARAM_SPEC(g_object_class_find_property(gclass, "pixel-format")));

    // 3 = resolution
    static const GEnumValue res_values[] = {
        {static_cast<int>(ResolutionEnum::DIM_HD), "HD", "HD"},
        {static_cast<int>(ResolutionEnum::DIM_FHD), "FHD", "FHD"},
        {static_cast<int>(ResolutionEnum::DIM_2K), "R2K", "R2K"},
        {static_cast<int>(ResolutionEnum::DIM_4K), "R4K", "R4K"},
        {0, nullptr, nullptr}};
    GType res_type = g_enum_register_static("ResolutionEnum", res_values);
    g_object_class_install_property(
        gclass,
        3,
        g_param_spec_enum("resolution", "Resolution", "HD / FHD / 2K / 4K", res_type,
                          static_cast<int>(DEFAULT_RESOLUTION),
                          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_assert(G_IS_PARAM_SPEC(g_object_class_find_property(gclass, "resolution")));

    // 4 = framerate
    static const GEnumValue fps_values[] = {
        {static_cast<int>(FPSEnum::FPS_15), "15", "15fps"},
        {static_cast<int>(FPSEnum::FPS_30), "30", "30fps"},
        {static_cast<int>(FPSEnum::FPS_60), "60", "60fps"},
        {0, nullptr, nullptr}};
    GType fps_type = g_enum_register_static("FPSEnum", fps_values);
    g_object_class_install_property(
        gclass,
        4,
        g_param_spec_enum(
            "fps",
            "FPS",
            "Frames per second",
            fps_type,
            static_cast<int>(DEFAULT_FPS),
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_assert(G_IS_PARAM_SPEC(g_object_class_find_property(gclass, "fps")));

    // 5 = buffer-count
    g_object_class_install_property(
        gclass,
        5,
        g_param_spec_uint(
            "buffer-count",
            "Buffer Count",
            "Number of MMAP buffers",
            2, 32, DEFAULT_BUFFER_COUNT,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    // metadata and pads
    gst_element_class_set_static_metadata(
        eclass,
        " V4L2 Source",
        "Source/Video",
        "Wraps ::V4L2Camera as GstPushSrc",
        "You <you@example.com>");
    gst_element_class_add_static_pad_template(eclass, &pad_template);

    // override base class virtuals
    auto *bclass = GST_BASE_SRC_CLASS(klass);
    bclass->start = _v4l2src_start;
    bclass->stop = _v4l2src_stop;
    bclass->get_caps = _v4l2src_get_caps;
    bclass->negotiate = _v4l2src_negotiate;
    bclass->fixate = _v4l2src_fixate;

    // PushSrc virtual method
    auto *pclass = GST_PUSH_SRC_CLASS(klass);
    pclass->create = _v4l2src_create;
}

static void _v4l2src_init(V4L2Src *self)
{
    self->device_path = g_strdup(DEFAULT_DEVICE_PATH);
    self->pixel_format = DEFAULT_PIXEL_FORMAT;
    self->resolution = DEFAULT_RESOLUTION;
    self->fps = DEFAULT_FPS;
    self->buffer_count = DEFAULT_BUFFER_COUNT;
    self->frame_number = 0;

    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}

static void _v4l2src_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(object));
    switch (prop_id)
    {
    case 1: // device
        g_free(self->device_path);
        self->device_path = g_value_dup_string(value);
        break;
    case 2: // pixel-format
        self->pixel_format = static_cast<PixelFormatEnum>(g_value_get_enum(value));
        break;
    case 3: // resolution ← now correctly registered
        self->resolution = static_cast<ResolutionEnum>(g_value_get_enum(value));
        break;
    case 4: // fps
        self->fps = static_cast<FPSEnum>(g_value_get_enum(value));
        break;
    case 5: // buffer-count
        self->buffer_count = g_value_get_uint(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void _v4l2src_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(object));
    switch (prop_id)
    {
    case 1:
        g_value_set_string(value, self->device_path);
        break;
    case 2:
        g_value_set_enum(value, static_cast<gint>(self->pixel_format));
        break;
    case 3:
        g_value_set_enum(value, static_cast<gint>(self->resolution));
        break;
    case 4:
        g_value_set_enum(value, static_cast<gint>(self->fps));
        break;
    case 5:
        g_value_set_uint(value, self->buffer_count);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

[[nodiscard]] static GstCaps *get_active_caps(const V4L2Src *self)
{
    const auto [w, h] = v4l2::dimensions_decompress(static_cast<uint32_t>(self->resolution));
    const gint fpsn = static_cast<gint>(self->fps);
    const gint fpsd = 1;

    GstCaps *caps = nullptr;

    if (self->pixel_format == PixelFormatEnum::MJPG)
    {
        caps = gst_caps_new_simple("image/jpeg",
                                   "width", G_TYPE_INT, w,
                                   "height", G_TYPE_INT, h,
                                   "framerate", GST_TYPE_FRACTION, fpsn, fpsd,
                                   nullptr);
    }
    else if (self->pixel_format == PixelFormatEnum::YUYV)
    {
        caps = gst_caps_new_simple("video/x-raw",
                                   "format", G_TYPE_STRING, "YUY2",
                                   "width", G_TYPE_INT, w,
                                   "height", G_TYPE_INT, h,
                                   "framerate", GST_TYPE_FRACTION, fpsn, fpsd,
                                   nullptr);
    }
    else
    {
        GST_ERROR("get_active_caps: invalid pixel format enum = %d", static_cast<int>(self->pixel_format));
        return nullptr;
    }

    // attempt to fixate
    GstCaps *fixed = gst_caps_fixate(gst_caps_copy(caps));
    gst_caps_unref(caps);

    if (!fixed || !gst_caps_is_fixed(fixed))
    {
        GST_ERROR("get_active_caps: fixation failed or result is not fixed");
        if (fixed)
            gst_caps_unref(fixed);
        return nullptr;
    }

    return fixed;
}

[[nodiscard]] std::optional<PixelFormatEnum> to_pixel_format(gint value)
{
    switch (static_cast<PixelFormatEnum>(value))
    {
    case PixelFormatEnum::MJPG:
        return PixelFormatEnum::MJPG;
    case PixelFormatEnum::YUYV:
        return PixelFormatEnum::YUYV;
    default:
        fmt::print(stderr, "invalid pixel format value: {}\n", value);
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<FPSEnum> to_fps_enum(gint value)
{
    switch (static_cast<FPSEnum>(value))
    {
    case FPSEnum::FPS_15:
        return FPSEnum::FPS_15;
    case FPSEnum::FPS_30:
        return FPSEnum::FPS_30;
    case FPSEnum::FPS_60:
        return FPSEnum::FPS_60;
    default:
        fmt::print(stderr, "invalid fps enum: {}\n", value);
        return std::nullopt;
    }
}

static gboolean _v4l2src_start(GstBaseSrc *basesrc)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(basesrc)); // for GstBaseSrc*
    v4l2::V4l2Config cfg;
    cfg.device_path_ = self->device_path;
    auto maybe_fmt = to_pixel_format(static_cast<gint>(self->pixel_format));
    if (!maybe_fmt)
    {
        fmt::print(stderr, "ERROR: pixel format enum conversion failed: {}\n", static_cast<int>(self->pixel_format));
        GST_ERROR_OBJECT(self, "invalid pixel format: %d", static_cast<int>(self->pixel_format));
        return FALSE;
    }
    cfg.format_ = *maybe_fmt;
    cfg.dimension_ = static_cast<ResolutionEnum>(self->resolution);
    auto maybe_fps = to_fps_enum(static_cast<gint>(self->fps));
    if (!maybe_fps)
    {
        GST_ERROR_OBJECT(self, "invalid FPS enum: %d", static_cast<int>(self->fps));
        return FALSE;
    }
    cfg.fps_num_ = *maybe_fps;

    cfg.buffer_count_ = self->buffer_count;

    auto [width, height] = v4l2::dimensions_decompress(static_cast<uint32_t>(cfg.dimension_));
    fmt::print("DEBUG: Starting _v4l2src with:\n"
               "  device_path: {}\n"
               "  pixel_format: {} ({:08X})\n"
               "  resolution: {}x{}\n"
               "  fps: {}\n"
               "  buffer_count: {}\n",
               cfg.device_path_,
               static_cast<uint32_t>(cfg.format_), static_cast<uint32_t>(cfg.format_),
               width, height,
               static_cast<uint32_t>(cfg.fps_num_),
               cfg.buffer_count_);

    self->camera = std::make_unique<v4l2::V4L2Camera>(cfg);

    try
    {
        self->camera->open_device();
        self->camera->configure();
        self->camera->start_streaming();
    }
    catch (const std::exception &ex)
    {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("Failed to start camera"), ("%s", ex.what()));
        self->camera.reset();

        return FALSE;
    }

    self->frame_number = 0;

    // ✅ NO CAPS SETTING HERE.
    // let negotiate() figure it out like a grown up
    if (!gst_base_src_negotiate(GST_BASE_SRC(self)))
    {
        GST_ERROR_OBJECT(self, "negotiation failed");
        return FALSE;
    }

    return TRUE;
}

static gboolean _v4l2src_stop(GstBaseSrc *basesrc)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(basesrc));
    fmt::print(stderr, "🛑 _v4l2src_stop() called — cleanup engaged\n");

    if (self->camera)
    {
        try
        {
            self->camera->stop_streaming();
        }
        catch (const std::exception &ex)
        {
            fmt::print(stderr, "💥 stop_streaming threw: {}\n", ex.what());
        }

        self->camera.reset(); // 🧹 clear it for real
    }

    return TRUE;
}

static GstCaps *build_caps()
{
    GstCaps *caps = gst_caps_new_empty();

    for (auto format : {PixelFormatEnum::MJPG, PixelFormatEnum::YUYV})
    {
        for (auto res : {ResolutionEnum::DIM_HD, ResolutionEnum::DIM_FHD, ResolutionEnum::DIM_2K, ResolutionEnum::DIM_4K})
        {
            for (auto fps : {FPSEnum::FPS_15, FPSEnum::FPS_30, FPSEnum::FPS_60})
            {
                const auto [w, h] = v4l2::dimensions_decompress(static_cast<uint32_t>(res));
                gint fpsn = static_cast<gint>(fps), fpsd = 1;

                GstStructure *s = nullptr;

                if (format == PixelFormatEnum::MJPG)
                {
                    s = gst_structure_new("image/jpeg",
                                          // must match static pad: name + type + value
                                          "memory", G_TYPE_STRING, "NVMM",
                                          "width", G_TYPE_INT, w,
                                          "height", G_TYPE_INT, h,
                                          "framerate", GST_TYPE_FRACTION, fpsn, fpsd,
                                          nullptr);
                }
                else
                {
                    s = gst_structure_new("video/x-raw",
                                          "format", G_TYPE_STRING, "YUY2",
                                          "width", G_TYPE_INT, w,
                                          "height", G_TYPE_INT, h,
                                          "framerate", GST_TYPE_FRACTION, fpsn, fpsd,
                                          nullptr);
                }

                gst_caps_append_structure(caps, s);
            }
        }
    }

    return caps;
}

// Convert our FourCC enum into a GstVideoFormat for gst_buffer_add_video_meta()
static GstVideoFormat to_gst_video_format(PixelFormatEnum fmt)
{
    switch (fmt)
    {
    case PixelFormatEnum::YUYV:
        return GST_VIDEO_FORMAT_YUY2;
    case PixelFormatEnum::MJPG:
        // MJPEG isn’t raw planar—if you really want metadata you'll
        // need to decode first. But GStreamer does define an MJPEG enum:
        return GST_VIDEO_FORMAT_I420;
    default:
        return GST_VIDEO_FORMAT_UNKNOWN;
    }
}

static GstCaps *_v4l2src_get_caps([[maybe_unused]] GstBaseSrc *basesrc, GstCaps *filter)
{
    static GstCaps *caps_cache = build_caps(); // build once, never again

    if (filter)
    {
        return gst_caps_intersect_full(
            gst_caps_ref(filter),
            gst_caps_ref(caps_cache),
            GST_CAPS_INTERSECT_FIRST);
    }

    return gst_caps_ref(caps_cache);
}

static GstFlowReturn _v4l2src_create(GstPushSrc *push, GstBuffer **outbuf)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(push));
    fmt::print(stderr, "🟡 ENTER: _v4l2src_create()\n");

    // 1) dequeue a frame (records last_buf_index_ in the camera)
    v4l2::FrameView view{};
    try
    {
        view = self->camera->capture_frame();
    }
    catch (const std::exception &e)
    {
        GST_ERROR_OBJECT(self, "capture_frame failed: %s", e.what());
        return GST_FLOW_ERROR;
    }

    // 🔐 sanity check: driver gave us trash bytesused
    if (view.image.empty() || view.image.size_bytes() > 16 * 1024 * 1024)
    {
        fmt::print(stderr, "💩 invalid image from capture_frame. size={} bytes\n", view.image.size_bytes());
        GST_ERROR_OBJECT(self, "invalid image size from V4L2 driver: %zu", view.image.size_bytes());
        return GST_FLOW_ERROR;
    }

    fmt::print(stderr, "📦 valid image captured: {}x{} @ {} bytes\n",
               view.width, view.height, view.image.size_bytes());

    auto *ptr = const_cast<std::byte *>(view.image.data());
    auto size = view.image.size_bytes();

    // 2) wrap that mmap’d pointer in a GstBuffer.
    //    We pass the raw camera pointer as user_data, and in destroy
    //    we call release_frame() to re-queue it back to V4L2.
    GstBuffer *buf = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        static_cast<gpointer>(ptr), size,
        0, size,
        self->camera.get(),
        [](gpointer user_data)
        {
            try
            {
                auto *cam = static_cast<v4l2::V4L2Camera *>(user_data);
                if (cam->has_valid_frame())
                {
                    fmt::print(stderr, "🔁 requeuing buffer back to camera\n");
                    cam->release_frame();
                }
                else
                {
                    fmt::print(stderr, "⚠️ no valid buffer to release\n");
                }
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr, "❌ release_frame EXCEPTION: {}\n", e.what());
            }
            catch (...)
            {
                fmt::print(stderr, "💥 unknown exception in release_frame\n");
            }
        });

    GstClockTime dur = ns_per_frame(self->fps);
    GST_BUFFER_DURATION(buf) = dur;
    GST_BUFFER_OFFSET(buf) = self->frame_number;
    GST_BUFFER_OFFSET_END(buf) = self->frame_number + 1;
    self->frame_number++;
    GST_BUFFER_PTS(buf) = view.v4l2_timestamp_us * 1000; // 💡 nsec

    fmt::print(stderr, "🟢 pushing buffer: pts={} dur={} offset={}\n",
               GST_BUFFER_PTS(buf), dur, GST_BUFFER_OFFSET(buf));

    // If downstream expects video metadata, attach it now:
    GstVideoFormat vf = to_gst_video_format(self->pixel_format);
    if (vf != GST_VIDEO_FORMAT_UNKNOWN && vf != GST_VIDEO_FORMAT_I420)
    {
        gst_buffer_add_video_meta(
            buf,
            GST_VIDEO_FRAME_FLAG_NONE,
            vf,
            view.width,
            view.height);
    }
    *outbuf = buf;
    return GST_FLOW_OK;
}

static void _v4l2src_finalize(GObject *object)
{
    auto *self = get_instance<V4L2Src>(G_OBJECT(object));
    g_free(self->device_path);
    G_OBJECT_CLASS(_v4l2src_parent_class)->finalize(object);
    self->camera.reset();
}

gboolean plugin_init(GstPlugin *p)
{
    // these must be called BEFORE gstreamer touches any internal global state
    g_setenv("GST_REGISTRY_UPDATE", "no", TRUE);
    g_setenv("GST_REGISTRY_FORK", "no", TRUE);

    return gst_element_register(p, "v4l2-src", GST_RANK_NONE, GST_TYPE_V4L2SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    v4l2src,
    "V4L2 Source",
    plugin_init,
    "1.0",
    "LGPL",
    "v4l2-src",
    "http://example.com")
