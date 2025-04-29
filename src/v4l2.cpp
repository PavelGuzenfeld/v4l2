#include <array>
#include <cerrno>
#include <cstring>
#include <ctime> // For clock_gettime
#include <fcntl.h>
#include <fmt/core.h>
#include <linux/videodev2.h>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "v4l2/v4l2.hpp"

#ifndef V4L2_CID_TIMESTAMP_SOURCE
#define V4L2_CID_TIMESTAMP_SOURCE (V4L2_CID_USER_BASE + 0x1029)
#endif

#ifndef V4L2_TIMESTAMP_SRC_EOF
#define V4L2_TIMESTAMP_SRC_EOF 0
#define V4L2_TIMESTAMP_SRC_SOE 1
#endif

namespace v4l2
{
    V4L2Camera::V4L2Camera(const V4l2Config &config)
        : config_(config),
          fd_(-1),
          configured_(false),
          last_buf_index_(std::nullopt),
          buffers_(config_.buffer_count_),
          caps_{}
    {
    }

    V4L2Camera::~V4L2Camera() noexcept
    {
        cleanup();
    }

    V4L2Camera::V4L2Camera(V4L2Camera &&other) noexcept
        : config_(std::move(other.config_)),
          fd_(other.fd_),
          last_buf_index_(std::move(other.last_buf_index_)),
          caps_(std::move(other.caps_))
    {
        for (std::size_t i = 0; i < config_.buffer_count_; ++i)
        {
            buffers_[i] = other.buffers_[i];
        }
        other.fd_ = -1;
    }

    V4L2Camera &V4L2Camera::operator=(V4L2Camera &&other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            config_ = std::move(other.config_);
            fd_ = other.fd_;
            last_buf_index_ = std::move(other.last_buf_index_);
            caps_ = std::move(other.caps_);
            for (std::size_t i = 0; i < config_.buffer_count_; ++i)
            {
                buffers_[i] = other.buffers_[i];
            }
            other.fd_ = -1;
        }
        return *this;
    }

    void V4L2Camera::open_device()
    {
        fd_ = open(config_.device_path_.c_str(), O_RDWR);
        if (fd_ < 0)
        {
            if (errno == EBUSY)
            {
                fmt::print(stderr, "device already in use: {}\n", config_.device_path_);
            }
            auto const msg = fmt::format("Failed to open device: {}\n", strerror(errno));
            throw std::runtime_error(msg);
        }

        v4l2_capability cap{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
        {
            auto const msg = fmt::format("VIDIOC_QUERYCAP failed: {}\n", strerror(errno));
            throw std::runtime_error(msg);
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {
            auto const msg = fmt::format("Device does not support video capture\n");
            throw std::runtime_error(msg);
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING))
        {
            auto const msg = fmt::format("Device doesn't support streaming I/O\n");
            throw std::runtime_error(msg);
        }

        caps_ = V4lCaps{
            .driver = std::string(reinterpret_cast<const char *>(cap.driver), strnlen(reinterpret_cast<const char *>(cap.driver), sizeof(cap.driver))),
            .card = std::string(reinterpret_cast<const char *>(cap.card), strnlen(reinterpret_cast<const char *>(cap.card), sizeof(cap.card)))};
    }

    [[nodiscard]] bool V4L2Camera::try_soe() noexcept
    {
        v4l2_queryctrl qctrl{};
        qctrl.id = V4L2_CID_TIMESTAMP_SOURCE;
        if (ioctl(fd_, VIDIOC_QUERYCTRL, &qctrl) == 0)
        {
            v4l2_control ctrl{};
            ctrl.id = V4L2_CID_TIMESTAMP_SOURCE;
            ctrl.value = V4L2_TIMESTAMP_SRC_SOE;
            if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) == 0)
            {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] constexpr std::string_view fourcc_str(std::uint32_t fourcc)
    {
        static thread_local char str[5];
        str[0] = static_cast<char>(fourcc & 0xFF);
        str[1] = static_cast<char>((fourcc >> 8) & 0xFF);
        str[2] = static_cast<char>((fourcc >> 16) & 0xFF);
        str[3] = static_cast<char>((fourcc >> 24) & 0xFF);
        str[4] = '\0';
        return str;
    }

    void V4L2Camera::configure()
    {
        if (fd_ < 0 || configured_)
        {
            return;
        }

        const auto [width, height] = dimensions_decompress(static_cast<std::uint32_t>(config_.dimension_));

        // ✍️ validate format up front
        const std::uint32_t requested_fourcc = static_cast<std::uint32_t>(config_.format_);
        if (requested_fourcc != V4L2_PIX_FMT_MJPEG && requested_fourcc != V4L2_PIX_FMT_YUYV)
        {
            throw std::invalid_argument(fmt::format("Unsupported pixel format: fourcc={:08X}", requested_fourcc));
        }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = requested_fourcc;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        fmt.fmt.pix.bytesperline = 0;
        fmt.fmt.pix.sizeimage = 0;
        fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
        {
            if (errno == EBUSY)
            {
                fmt::print(stderr, "device busy during VIDIOC_S_FMT: {}\n", config_.device_path_);
            }
            throw std::runtime_error(fmt::format("VIDIOC_S_FMT failed: {}", strerror(errno)));
        }

        // 🧠 validate driver didn't mess us
        if (fmt.fmt.pix.pixelformat != requested_fourcc)
        {
            throw std::runtime_error(fmt::format(
                "driver rejected pixel format: requested '{}', got '{}'",
                fourcc_str(requested_fourcc),
                fourcc_str(fmt.fmt.pix.pixelformat)));
        }
        fmt::print("NEGOTIATED PIXEL FORMAT: {}\n", fourcc_str(fmt.fmt.pix.pixelformat));

        // update config with the confirmed format
        config_.format_ = static_cast<PixelFormat>(fmt.fmt.pix.pixelformat);

        // 🛠 verify format actually got set
        v4l2_format check_fmt{};
        check_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_FMT, &check_fmt) < 0)
        {
            throw std::runtime_error("VIDIOC_G_FMT failed after format negotiation");
        }

        if (check_fmt.fmt.pix.pixelformat != static_cast<std::uint32_t>(config_.format_))
        {
            throw std::runtime_error(fmt::format(
                "driver format mismatch: got '{}', expected '{}'",
                fourcc_str(check_fmt.fmt.pix.pixelformat),
                fourcc_str(static_cast<std::uint32_t>(config_.format_))));
        }

        // 🐢 fix FPS assignment: denominator should be FPS
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(config_.fps_num_);

        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
        {
            throw std::runtime_error(fmt::format("VIDIOC_S_PARM failed: {}", strerror(errno)));
        }

        // 🧽 request MMAP buffers
        v4l2_requestbuffers req{};
        req.count = config_.buffer_count_;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
        {
            throw std::runtime_error(fmt::format("VIDIOC_REQBUFS failed: {}", strerror(errno)));
        }

        // map buffers
        for (std::size_t i = 0; i < config_.buffer_count_; ++i)
        {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
            {
                throw std::runtime_error(fmt::format("VIDIOC_QUERYBUF failed for index {}: {}", i, strerror(errno)));
            }

            void *mapped = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (mapped == MAP_FAILED)
            {
                throw std::runtime_error(fmt::format("mmap failed at index {}: {}", i, strerror(errno)));
            }

            buffers_[i].data = static_cast<std::byte *>(mapped);
            buffers_[i].size = buf.length;
        }

        // enqueue
        for (std::size_t i = 0; i < config_.buffer_count_; ++i)
        {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            {
                throw std::runtime_error(fmt::format("VIDIOC_QBUF failed at index {}: {}", i, strerror(errno)));
            }
        }

        configured_ = true;
    }

    void V4L2Camera::start_streaming()
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
        {
            throw std::runtime_error("VIDIOC_STREAMON failed");
        }
    }

    [[nodiscard]] FrameView V4L2Camera::capture_frame()
    {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0)
        {
            throw std::runtime_error("VIDIOC_DQBUF failed");
        }

        if (buf.index >= buffers_.size())
        {
            throw std::runtime_error(fmt::format("DQBUF returned invalid buffer index: {}", buf.index));
        }

        last_buf_index_ = buf.index;

        // parse format & dimensions
        const auto &mapped = buffers_[buf.index];
        auto const [width, height] = dimensions_decompress(static_cast<uint32_t>(config_.dimension_));
        // Get the driver-provided timestamp in microseconds.
        const std::uint64_t v4l2_ts_us =
            static_cast<std::uint64_t>(buf.timestamp.tv_sec) * 1'000'000ULL + buf.timestamp.tv_usec;
        // Get current host monotonic time.
        // const std::uint64_t now_monotonic_us = get_monotonic_time_us();
        std::uint64_t const now_monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        return FrameView{
            .timestamp_monotonic_us = now_monotonic_us,
            .v4l2_timestamp_us = v4l2_ts_us,
            .image = std::span<std::byte const>(mapped.data, buf.bytesused),
            .width = width,
            .height = height,
            .format = config_.format_,
        };
    }

    void V4L2Camera::release_frame()
    {
        if (!last_buf_index_.has_value())
        {
            fmt::print(stderr, "WARN: release_frame called without valid last_buf_index_\n");
            return;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = *last_buf_index_;

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
        {
            throw std::runtime_error(fmt::format("VIDIOC_QBUF failed for index {}", buf.index));
        }

        last_buf_index_.reset();
    }

    void V4L2Camera::stop_streaming()
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0)
        {
            throw std::runtime_error("VIDIOC_STREAMOFF failed");
        }
    }

    void V4L2Camera::cleanup() noexcept
    {
        for (auto &buf : buffers_)
        {
            if (buf.is_valid())
            {
                munmap(buf.data, buf.size);
                buf.data = nullptr;
                buf.size = 0;
            }
        }

        if (fd_ >= 0)
        {
            close(fd_);
        }
        fd_ = -1;
    }

    V4lCaps V4L2Camera::get_caps() const noexcept
    {
        return caps_;
    }

    [[nodiscard]] bool MappedBuffer::is_valid() const noexcept
    {
        return data && data != MAP_FAILED;
    }

    [[nodiscard]] bool V4L2Camera::has_valid_frame() const noexcept
    {
        return last_buf_index_.has_value();
    }

} // namespace v4l2
