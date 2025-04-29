
#include <chrono>  // For std::chrono::time_point
#include <cstdint> // For uint32_t
#include <span>    // For std::span
#include <string>  // For std::string
#include <utility> // For std::pair
namespace v4l2
{
    enum class FPS : uint32_t
    {
        FPS_15 = 15,
        FPS_30 = 30,
        FPS_60 = 60
    };

    consteval uint32_t dimensions_compress(uint32_t width, uint32_t height) noexcept
    {
        return (width << 16) | height;
    }

    constexpr std::pair<uint32_t, uint32_t> dimensions_decompress(uint32_t dimensions) noexcept
    {
        return {dimensions >> 16, dimensions & 0xFFFF};
    }

    constexpr uint32_t dimensions_size(std::pair<uint32_t, uint32_t> dimensions) noexcept
    {
        return dimensions.first * dimensions.second;
    }
    enum class PixelDimension : uint32_t
    {
        DIM_HD = dimensions_compress(1280, 720),
        DIM_FHD = dimensions_compress(1920, 1080),
        DIM_2K = dimensions_compress(2048, 1080),
        DIM_4K = dimensions_compress(3840, 2160),
    };
    static_assert(dimensions_decompress(static_cast<uint32_t>(PixelDimension::DIM_HD)) == std::pair<uint32_t, uint32_t>{1280, 720});
    static_assert(dimensions_decompress(static_cast<uint32_t>(PixelDimension::DIM_FHD)) == std::pair<uint32_t, uint32_t>{1920, 1080});
    static_assert(dimensions_decompress(static_cast<uint32_t>(PixelDimension::DIM_2K)) == std::pair<uint32_t, uint32_t>{2048, 1080});
    static_assert(dimensions_decompress(static_cast<uint32_t>(PixelDimension::DIM_4K)) == std::pair<uint32_t, uint32_t>{3840, 2160});

    [[nodiscard]] consteval std::uint32_t make_fourcc(char a, char b, char c, char d) noexcept
    {
        return static_cast<std::uint32_t>(a) |
               (static_cast<std::uint32_t>(b) << 8) |
               (static_cast<std::uint32_t>(c) << 16) |
               (static_cast<std::uint32_t>(d) << 24);
    }

    enum class PixelFormat : std::uint32_t
    {
        MJPG = make_fourcc('M', 'J', 'P', 'G'),
        YUYV = make_fourcc('Y', 'U', 'Y', 'V')
    };

    static_assert(static_cast<std::uint32_t>(PixelFormat::MJPG) == 0x47504A4D);
    static_assert(static_cast<std::uint32_t>(PixelFormat::YUYV) == 0x56595559);

    struct V4l2Config
    {
        std::string device_path_ = "/dev/video0";
        PixelDimension dimension_ = PixelDimension::DIM_4K;
        PixelFormat format_ = PixelFormat::MJPG;
        FPS fps_num_ = FPS::FPS_30;
        uint32_t buffer_count_ = 4;
    };

    struct V4lCaps
    {
        std::string driver;
        std::string card;
    };

    struct FrameView
    {
        uint64_t timestamp_monotonic_us{};
        uint64_t v4l2_timestamp_us{};
        std::span<std::byte const> image;
        std::uint32_t width{};
        std::uint32_t height{};
        PixelFormat format{};
    };

    struct MappedBuffer
    {
        std::byte *data{};
        std::size_t size{};

        FrameView to_frame(std::uint32_t width, std::uint32_t height, std::uint32_t bytes_per_line, PixelFormat format) noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
    };

} // namespace v4l2
