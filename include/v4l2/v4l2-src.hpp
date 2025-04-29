#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#include <gst/base/gstpushsrc.h>
#include <linux/videodev2.h> // For fourcc constants
#include <memory>
#pragma GCC diagnostic pop
#include "v4l2.hpp"

G_BEGIN_DECLS

#define PACKAGE "v4l2-src"

// Enumerations for GObject properties

// plugin types
static_assert(static_cast<std::uint32_t>(v4l2::PixelFormat::MJPG) == V4L2_PIX_FMT_MJPEG, "MJPG mismatch");
using PixelFormatEnum = v4l2::PixelFormat;
using ResolutionEnum = v4l2::PixelDimension;
using FPSEnum = v4l2::FPS;

// Default values for properties
constexpr auto DEFAULT_DEVICE_PATH = "/dev/video0";
constexpr char PAD_CAPS[] =
    "video/x-raw,format=(string)YUY2,width=(int)[1,MAX],height=(int)[1,MAX],framerate=(fraction)[0/1,MAX];"
    "image/jpeg,width=(int)[1,MAX],height=(int)[1,MAX],framerate=(fraction)[0/1,MAX]";
constexpr auto DEFAULT_PIXEL_FORMAT = PixelFormatEnum::MJPG;
constexpr auto DEFAULT_RESOLUTION = ResolutionEnum::DIM_HD;
constexpr auto DEFAULT_FPS = FPSEnum::FPS_30;
constexpr guint DEFAULT_BUFFER_COUNT = 2u;

// GObject type and casting macros
#define GST_TYPE_V4L2SRC (v4l2src_get_type())
#define GST_V4L2SRC(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_V4L2SRC, V4L2Src))
#define GST_V4L2SRC_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_V4L2SRC, V4L2SrcClass))
#define GST_IS_V4L2SRC(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_V4L2SRC))
#define GST_IS_V4L2SRC_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_V4L2SRC))

// Forward declarations

typedef struct _V4L2Src V4L2Src;
typedef struct _V4L2SrcClass V4L2SrcClass;

struct _V4L2Src
{
    GstPushSrc parent;
    gchar *device_path;
    PixelFormatEnum pixel_format;
    ResolutionEnum resolution;
    FPSEnum fps;
    guint buffer_count;
    std::unique_ptr<v4l2::V4L2Camera> camera;
    std::uint64_t frame_number;
};

struct _V4L2SrcClass
{
    GstPushSrcClass parent_class;
};

GType v4l2src_get_type(void);

// Plugin entry point
gboolean plugin_init(GstPlugin *plugin);

G_END_DECLS
