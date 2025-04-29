#pragma once
#include "definitions.hpp"
#include "exception-rt/exception.hpp" // For exception
#include <cstdint>                    // For uint64_t, uint32_t, uint8_t
#include <memory>                     // For std::unique_ptr
#include <optional>                   // For std::optional
#include <span>                       // For std::span
#include <string>                     // For std::string
#include <vector>                     // For std::vector

namespace v4l2
{
    class [[nodiscard]] V4L2Camera final
    {
    public:
        using Buffer = std::span<uint8_t>;

        explicit V4L2Camera(const V4l2Config &config);
        ~V4L2Camera() noexcept;

        // No copy semantics
        V4L2Camera(const V4L2Camera &) = delete;
        V4L2Camera &operator=(const V4L2Camera &) = delete;
        // Move semantics
        V4L2Camera(V4L2Camera &&) noexcept;
        V4L2Camera &operator=(V4L2Camera &&) noexcept;

        /*
         * Open the device and configure it.
         * Throws std::runtime_error on failure.
         */
        void open_device();
        /*
         * Configure the device.
         * Throws std::runtime_error on failure.
         */

        [[nodiscard]] bool try_soe() noexcept;
        void configure();
        /*
         * Start streaming.
         * Throws std::runtime_error on failure.
         */
        void start_streaming();

        /*
         * Capture a frame.
         * Returns a FrameView that holds a span into the internal buffer.
         * Throws std::runtime_error on failure.
         */
        [[nodiscard]] FrameView capture_frame();

        /*
         * Release the frame back to the driver.
         * Throws std::runtime_error on failure.
         */
        void release_frame();

        void stop_streaming();
        V4lCaps get_caps() const noexcept;

        [[nodiscard]] bool has_valid_frame() const noexcept;

    private:
        void cleanup() noexcept;

    private:
        V4l2Config config_;
        int fd_;
        bool configured_;
        std::optional<uint32_t> last_buf_index_;
        std::vector<MappedBuffer> buffers_;
        V4lCaps caps_;
    };
} // namespace v4l2
