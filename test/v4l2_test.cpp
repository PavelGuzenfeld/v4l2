#include "v4l2/v4l2.hpp"
#include <cassert>    // For assert
#include <fmt/core.h> // For fmt::format

void create_destroy()
{
    fmt::print("Create and destroy\n");
    v4l2::V4l2Config config{};

    v4l2::V4L2Camera camera(config);
    camera.open_device();
    camera.configure();
    (void)camera.try_soe(); // nodiscard
    camera.start_streaming();
    (void)camera.capture_frame();
    camera.release_frame();
    camera.stop_streaming();
    fmt::print("Create and destroy done\n");
}

void multiple_lifecycles()
{
    fmt::print("Multiple lifecycles\n");
    for (int i = 0; i < 10; ++i)
    {
        v4l2::V4L2Camera cam(v4l2::V4l2Config{});
        cam.open_device();
        cam.configure();
        (void)cam.try_soe();
        cam.start_streaming();
        (void)cam.capture_frame();
        cam.release_frame();
        cam.stop_streaming();
    }
    fmt::print("Multiple lifecycles done\n");
}
void bad_device_path()
{
    try
    {
        v4l2::V4L2Camera cam(v4l2::V4l2Config{.device_path_ = "/dev/notreal"});
        cam.open_device();
        assert(false && "should have thrown");
    }
    catch (const std::exception &e)
    {
        fmt::print("Caught expected exception: {}\n", e.what());
    }
}

void test_get_frame()
{
    v4l2::V4l2Config config{};
    config.buffer_count_ = 1;

    v4l2::V4L2Camera cam(config);
    cam.open_device();
    cam.configure();
    cam.start_streaming();

    (void)cam.capture_frame();
    cam.release_frame();
    assert(frame.image.size() > 0);

    cam.stop_streaming();
}

void test_timestamp_diff()
{
    fmt::print("Testing timestamp diff\n");

    v4l2::V4l2Config config{};
    config.buffer_count_ = 1;

    v4l2::V4L2Camera cam(config);
    cam.open_device();
    cam.configure();
    (void)cam.try_soe();
    cam.start_streaming();

    constexpr int num_frames = 10;
    for (int i = 0; i < num_frames; ++i)
    {
        auto const frame = cam.capture_frame();
        cam.release_frame();

        // driver-provided timestamp (from buffer metadata)
        double drv_sec = static_cast<double>(frame.v4l2_timestamp_us) / 1'000'000.0;

        // system monotonic timestamp (from std::chrono)
        struct timespec ts_now{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts_now) != 0)
        {
            std::perror("clock_gettime");
            std::exit(EXIT_FAILURE);
        }
        double sys_sec = static_cast<double>(ts_now.tv_sec) + static_cast<double>(ts_now.tv_nsec) / 1e9;

        double offset_ms = (sys_sec - drv_sec) * 1000.0;

        fmt::print("[Frame {}] driver = {:.6f} s, sys = {:.6f} s, offset = {:.3f} ms\n",
                   i, drv_sec, sys_sec, offset_ms);

        if (std::abs(offset_ms) > 1000.0)
        {
            fmt::print("⚠️  suspicious offset: {:.3f} ms — check your camera or USB controller\n", offset_ms);
        }
    }

    cam.stop_streaming();
    fmt::print("Timestamp diff test done\n");
}

int main()
{
    fmt::print("Starting tests\n");
    test_timestamp_diff();
    create_destroy();
    multiple_lifecycles();
    test_get_frame();
    bad_device_path();
    fmt::print("Success\n");
    return 0;
}
