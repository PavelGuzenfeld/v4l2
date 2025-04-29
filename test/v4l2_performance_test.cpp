#include "v4l2/v4l2.hpp"
#include <algorithm>     // For std::min/max_element
#include <cassert>       // For assert
#include <chrono>        // For std::chrono::*
#include <cstdint>       // For std::uint64_t
#include <fmt/core.h>    // For fmt::format
#include <fstream>       // For std::ifstream, std::ofstream
#include <memory>        // For std::unique_ptr
#include <numeric>       // For std::accumulate
#include <sstream>       // For std::istringstream
#include <stdexcept>     // For std::runtime_error
#include <string>        // For std::string
#include <unordered_set> // For std::unordered_set
#include <vector>        // For std::vector
#include <zlib.h>        // because even *your* hash logic shouldn't be trusted

// Structure defining the parameters for a camera test configuration
struct TestCase
{
    std::string label;
    v4l2::PixelDimension dimension;
    v4l2::PixelFormat format;
    v4l2::FPS fps;
    std::uint32_t buffer_count;
};

// Structure holding the results of a performance test run
struct TestResult
{
    TestCase test;                // The base configuration used for the test
    int num_cameras;              // How many cameras were active in this test
    double ms_per_capture_cycle;  // Avg time (ms) to capture one frame from *all* cameras
    double cpu_usage_percent;     // Overall CPU usage during the test
    double mbps;                  // Total data throughput (MB/s) across all cameras
    bool kernel_warnings;         // Flag indicating potential USB issues in dmesg
    std::size_t crc_unique_count; // Number of unique frame hashes observed
    double jitter_min_ms;         // Min time delta (ms) between frames (based on cam 0)
    double jitter_max_ms;         // Max time delta (ms) between frames (based on cam 0)
    double jitter_avg_ms;         // Avg time delta (ms) between frames (based on cam 0)
    double mem_usage_mb;          // Peak resident memory usage (MB) of the process
    double v4l2_interval_ms_avg;  // Avg diff (ms) between V4L2 timestamp and monotonic clock
};

// Function to print the collected test results in a formatted table
void print_results(const std::vector<TestResult> &results)
{
    fmt::print("{:<15} {:>4} {:>11} {:>6s} {:>5} {:>4} {:>13} {:>10} {:>8} {:>10} {:>10} {:>20} {:>10}  {:>10}\n",
               "Label", "NCam", "Resolution", "FPS", "Fmt", "Bufs", "Cycle Time", "CPU (%)", "Kernel", "MB/s", "CRC uniq", "Jitter (min/max/avg)", "RAM (MB)", "V4L2 Interval (ms)");

    fmt::print("{:-<190}\n", ""); // Increased width for the new column

    for (const auto &r : results)
    {
        auto [w, h] = v4l2::dimensions_decompress(static_cast<std::uint32_t>(r.test.dimension)); // Decompress dimensions
        const char *fmt_str = r.test.format == v4l2::PixelFormat::MJPG ? "MJPG" : "YUYV";

        fmt::print("{:<15} {:>4} {:>9} {:>6} {:>6s} {:>4} {:>13.3f} {:>10.1f} {:>8} {:>10.2f} {:>10} {:>6.2f}/{:>5.2f}/{:>5.2f} {:>10.2f}  {:>10.2f}\n",
                   r.test.label,
                   r.num_cameras,              // Print number of cameras
                   fmt::format("{}x{}", w, h), // Format dimensions here
                   static_cast<uint32_t>(r.test.fps),
                   fmt_str,
                   r.test.buffer_count,
                   r.ms_per_capture_cycle, // Changed column name
                   r.cpu_usage_percent,
                   r.kernel_warnings ? "WARN" : "-",
                   r.mbps,
                   fmt::format("{}/{}", r.crc_unique_count, 100 * r.num_cameras), // Adjusted total expected frames for CRC
                   r.jitter_min_ms,
                   r.jitter_max_ms,
                   r.jitter_avg_ms,
                   r.mem_usage_mb,
                   r.v4l2_interval_ms_avg);
    }
}

// Consolidated function to measure capture performance for one or more cameras
TestResult measure_capture_performance(const TestCase &base_test, const std::vector<std::string> &device_paths)
{
    assert(!device_paths.empty() && "Must provide at least one device path.");

    std::vector<std::unique_ptr<v4l2::V4L2Camera>> cameras;
    cameras.reserve(device_paths.size());

    fmt::print("  Configuring {} camera(s) for test '{}':\n", device_paths.size(), base_test.label);

    // --- Setup Cameras ---
    try
    {
        for (const auto &path : device_paths)
        {
            v4l2::V4l2Config config{};
            config.device_path_ = path;
            config.dimension_ = base_test.dimension;
            config.format_ = base_test.format;
            config.fps_num_ = base_test.fps;
            config.buffer_count_ = base_test.buffer_count;

            auto [w, h] = v4l2::dimensions_decompress(static_cast<std::uint32_t>(config.dimension_)); // Decompress here for logging

            fmt::print("    - Device: {}, Res: {}, Fmt: {}, FPS: {}, Bufs: {}\n",
                       path,
                       fmt::format("{}x{}", w, h), // Format dimensions here
                       config.format_ == v4l2::PixelFormat::MJPG ? "MJPG" : "YUYV",
                       static_cast<uint32_t>(config.fps_num_),
                       config.buffer_count_);

            auto cam = std::make_unique<v4l2::V4L2Camera>(config);
            cam->open_device();
            cam->configure();
            cam->start_streaming();
            cameras.push_back(std::move(cam));
        }
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "ERROR: Failed to setup camera during configuration: {}\n", e.what());
        // Cleanup already opened cameras before re-throwing
        // Iterate by index to access device_path for potential error messages during cleanup
        for (size_t i = 0; i < cameras.size(); ++i)
        {
            if (cameras[i])
            { // Check if unique_ptr is not null
                try
                {
                    // Attempt stop_streaming without checking is_streaming()
                    cameras[i]->stop_streaming();
                }
                catch (const std::exception &cleanup_e)
                {
                    // Log cleanup error, using device_paths[i] for context
                    fmt::print(stderr, "WARN: Error stopping already configured camera {} during cleanup: {}\n", device_paths[i], cleanup_e.what());
                }
                catch (...)
                {
                    fmt::print(stderr, "WARN: Unknown error stopping already configured camera {} during cleanup.\n", device_paths[i]);
                }
            }
        }
        throw; // Re-throw the original exception to signal failure
    }

    // --- Prepare Measurement Variables ---
    auto cpu_stat_before = std::ifstream("/proc/stat");
    std::string _; // Dummy string to read "cpu" label
    long user1 = 0, nice1 = 0, system1 = 0, idle1 = 0;
    if (!(cpu_stat_before >> _ >> user1 >> nice1 >> system1 >> idle1))
    {
        fmt::print(stderr, "WARN: Could not read initial /proc/stat\n");
        // Handle error or default values if needed
    }

    constexpr int num_frames_per_camera = 100;
    std::size_t total_bytes = 0;                // Use std::size_t, usually unsigned long long
    std::vector<std::uint64_t> timestamps_cam0; // Timestamps from the *first* camera for jitter
    timestamps_cam0.reserve(num_frames_per_camera);
    std::unordered_set<std::uint32_t> crc_set;
    crc_set.reserve(num_frames_per_camera * cameras.size());
    std::vector<std::uint64_t> v4l2_intervals_us;
    v4l2_intervals_us.reserve(num_frames_per_camera * cameras.size());

    fmt::print("  Starting capture loop ({} frames per camera)...\n", num_frames_per_camera);
    const auto start = std::chrono::high_resolution_clock::now();

    // --- Capture Loop ---
    try
    {
        for (int i = 0; i < num_frames_per_camera; ++i)
        {
            for (size_t cam_idx = 0; cam_idx < cameras.size(); ++cam_idx)
            {
                // Added check if camera pointer is valid, although setup should ensure this
                if (!cameras[cam_idx])
                    continue;

                auto const frame = cameras[cam_idx]->capture_frame();
                cameras[cam_idx]->release_frame();

                v4l2_intervals_us.push_back(frame.timestamp_monotonic_us - frame.v4l2_timestamp_us);
                total_bytes += frame.image.size(); // Safe: size_t += size_t

                // Record timestamp for jitter analysis (only from the first camera)
                if (cam_idx == 0)
                {
                    timestamps_cam0.push_back(frame.v4l2_timestamp_us);
                }

                // Calculate CRC32 hash
                uLong crc = crc32(0L, Z_NULL, 0);
                // Ensure frame.image.data() and frame.image.size() are valid before hashing
                if (frame.image.data() != nullptr && frame.image.size() > 0)
                {
                    uLong hash = crc32(crc, reinterpret_cast<const Bytef *>(frame.image.data()), static_cast<uInt>(frame.image.size()));
                    crc_set.insert(static_cast<std::uint32_t>(hash));
                }
                else
                {
                    fmt::print(stderr, "WARN: Skipping CRC calculation for empty or null frame from camera index {}.\n", cam_idx);
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "ERROR: Failed during capture loop: {}\n", e.what());
        // Fall through to stop streaming and calculate partial results if possible
    }

    const auto end = std::chrono::high_resolution_clock::now();
    fmt::print("  Capture loop finished. Stopping streams...\n");

    // --- Stop Streaming ---
    // Iterate by index to use device_paths[i] in error messages
    for (size_t i = 0; i < cameras.size(); ++i)
    {
        try
        {
            if (cameras[i])
            { // Check if unique_ptr is valid
                // Attempt stop_streaming without checking is_streaming()
                cameras[i]->stop_streaming();
            }
        }
        catch (const std::exception &e)
        {
            // Use device_paths[i] for context in the error message
            fmt::print(stderr, "WARN: Error stopping camera stream for {}: {}\n", device_paths[i], e.what());
        }
        catch (...)
        {
            fmt::print(stderr, "WARN: Unknown error stopping camera stream for {}.\n", device_paths[i]);
        }
    }

    // --- Calculate Results ---
    fmt::print("  Calculating metrics...\n");

    // CPU Usage
    long user2 = 0, nice2 = 0, system2 = 0, idle2 = 0;
    double cpu_usage_percent = 0.0;
    auto cpu_stat_after = std::ifstream("/proc/stat");
    if (cpu_stat_after >> _ >> user2 >> nice2 >> system2 >> idle2)
    {
        // Use long long for intermediate calculations to avoid overflow before converting to double
        long long total1_ll = static_cast<long long>(user1) + nice1 + system1 + idle1;
        long long total2_ll = static_cast<long long>(user2) + nice2 + system2 + idle2;
        long long active1_ll = static_cast<long long>(user1) + nice1 + system1;
        long long active2_ll = static_cast<long long>(user2) + nice2 + system2;

        const double delta_total = static_cast<double>(total2_ll - total1_ll);
        const double delta_active = static_cast<double>(active2_ll - active1_ll);

        if (delta_total > 0)
        { // Avoid division by zero
            cpu_usage_percent = 100.0 * (delta_active / delta_total);
        }
        else
        {
            fmt::print(stderr, "WARN: No change detected in /proc/stat, CPU usage calculation might be inaccurate.\n");
        }
    }
    else
    {
        fmt::print(stderr, "WARN: Could not read final /proc/stat\n");
    }

    // Memory Usage (Peak Resident Set Size)
    double mem_usage_mb = 0.0;
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.rfind("VmRSS:", 0) == 0)
        {
            std::istringstream iss(line);
            std::string key, unit;
            long kb;
            if (iss >> key >> kb >> unit && unit == "kB")
            {
                mem_usage_mb = static_cast<double>(kb) / 1024.0; // convert kB to MB
            }
            else
            {
                fmt::print(stderr, "WARN: Could not parse VmRSS line: {}\n", line);
            }
            break;
        }
    }
    if (mem_usage_mb == 0.0)
    {
        fmt::print(stderr, "WARN: Could not read memory usage from /proc/self/status\n");
    }

    // Jitter (based on first camera's V4L2 timestamps)
    std::vector<double> diffs_ms;
    diffs_ms.reserve(timestamps_cam0.size() > 0 ? timestamps_cam0.size() - 1 : 0);
    for (std::size_t i = 1; i < timestamps_cam0.size(); ++i)
    {
        // Check for timestamp wrap-around or non-monotonicity (can happen!)
        if (timestamps_cam0[i] >= timestamps_cam0[i - 1])
        {
            std::uint64_t delta_us = timestamps_cam0[i] - timestamps_cam0[i - 1];
            diffs_ms.push_back(static_cast<double>(delta_us) / 1000.0); // convert us to ms
        }
        else
        {
            fmt::print(stderr, "WARN: Non-monotonic V4L2 timestamp detected for camera 0 ({} < {}). Skipping diff.\n", timestamps_cam0[i], timestamps_cam0[i - 1]);
        }
    }

    double jitter_min = 0.0, jitter_max = 0.0, jitter_avg = 0.0;
    if (!diffs_ms.empty())
    {
        jitter_min = *std::min_element(diffs_ms.begin(), diffs_ms.end());
        jitter_max = *std::max_element(diffs_ms.begin(), diffs_ms.end());
        // Ensure diffs_ms.size() > 0 before division
        jitter_avg = std::accumulate(diffs_ms.begin(), diffs_ms.end(), 0.0) / static_cast<double>(diffs_ms.size());
    }

    // Kernel Warnings
    // Use std::system carefully, consider alternatives if security is paramount
    bool kernel_warnings = system("dmesg | tail -n 100 | grep -qE 'usb.*(reset|error|fail|xhci.*(died|halt))'") == 0;
    if (kernel_warnings)
    {
        fmt::print("  WARN: Potential USB issues detected in recent dmesg output.\n");
    }

    // Timing and Throughput
    const auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    const double ms_per_capture_cycle = (num_frames_per_camera > 0) ? static_cast<double>(total_duration_ms) / num_frames_per_camera : 0.0;
    const double total_sec = static_cast<double>(total_duration_ms) / 1000.0;
    const double mbps = (total_sec > 0) ? (static_cast<double>(total_bytes) / 1'000'000.0) / total_sec : 0.0;

    // Average V4L2 Interval
    double v4l2_interval_ms_avg = 0.0;
    if (!v4l2_intervals_us.empty())
    {
        // Use double for sum to avoid potential overflow with uint64_t if many large intervals exist
        double sum_us = 0.0;
        for (std::uint64_t interval : v4l2_intervals_us)
        {
            sum_us += static_cast<double>(interval);
        }
        v4l2_interval_ms_avg = (sum_us / static_cast<double>(v4l2_intervals_us.size())) / 1000.0; // avg us -> ms
    }

    fmt::print("  Finished test '{}'.\n", base_test.label);

    return TestResult{
        base_test,
        static_cast<int>(cameras.size()), // Store number of cameras tested
        ms_per_capture_cycle,
        cpu_usage_percent,
        mbps,
        kernel_warnings,
        crc_set.size(),
        jitter_min,
        jitter_max,
        jitter_avg,
        mem_usage_mb,
        v4l2_interval_ms_avg};
}

// Function to run single-camera capture tests
void test_single_camera() // Renamed for clarity
{
    fmt::print("Measuring single camera capture performance\n");
    std::vector<TestCase> tests = {
        {"4K-MJPG-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"4K-MJPG-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"4K-MJPG-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 6},
        {"4K-MJPG-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 8},
        {"4K-MJPG-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"4K-MJPG-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
        {"4K-MJPG-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 6},
        {"4K-MJPG-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 8},
        {"FHD-MJPG-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"FHD-MJPG-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"FHD-MJPG-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 6},
        {"FHD-MJPG-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 8},
        {"FHD-MJPG-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"FHD-MJPG-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
        {"FHD-MJPG-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 6},
        {"FHD-MJPG-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 8}};

    std::vector<TestResult> results;
    results.reserve(tests.size());
    for (const auto &test : tests)
    {
        try
        {
            // Run the measurement function with a single device path
            results.push_back(measure_capture_performance(test, {"/dev/video0"}));
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "ERROR: Test '{}' failed: {}\n", test.label, e.what());
            // Optionally add a 'failed' result or skip adding to results
        }
    }

    print_results(results);
    fmt::print("Single camera performance measurement done\n\n");
}

// Function to run dual-camera capture tests
void test_dual_camera()
{
    fmt::print("Testing dual camera capture performance\n");
    std::vector<TestCase> tests = {
        {"DUAL-4k-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"DUAL-4k-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"DUAL-4k-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"DUAL-4k-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
        {"DUAL-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"DUAL-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"DUAL-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"DUAL-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4}};

    // Define the device paths for the dual cameras
    // *** IMPORTANT: Change these to match your system setup ***
    std::vector<std::string> dual_device_paths = {"/dev/video0", "/dev/video2"};
    if (dual_device_paths.size() < 2)
    { // Check if at least 2 paths are provided
        fmt::print(stderr, "WARN: Expected at least 2 device paths for dual camera test, found {}. Check 'dual_device_paths' variable.\n", dual_device_paths.size());
        // Decide how to proceed: skip, error out, or try with available paths?
        // For now, let's proceed, measure_capture_performance will handle the actual count.
    }

    std::vector<TestResult> results;
    results.reserve(tests.size());
    for (const auto &test : tests)
    {
        try
        {
            // Run the measurement function with the specified dual device paths
            results.push_back(measure_capture_performance(test, dual_device_paths));
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "ERROR: Dual Test '{}' failed: {}\n", test.label, e.what());
        }
    }

    print_results(results);
    fmt::print("Testing dual camera capture done\n\n");
}

// Function to run single-camera tests, potentially targeting a USB3 capable camera/port
void test_usb3_cam() // Could potentially use a different device path if needed
{
    fmt::print("Testing 'USB3' single camera performance (using /dev/video4)\n");
    // *** IMPORTANT: Ensure /dev/video0 is connected via USB3 for these tests to be meaningful ***
    std::vector<TestCase> tests = {
        {"USB3-4K-60", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_60, 2},
        {"USB3-4K-60", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_60, 4},
        {"USB3-4K-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"USB3-4K-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"USB3-4K-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"USB3-4K-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
        {"USB3-FHD-60", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_60, 2},
        {"USB3-FHD-60", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_60, 4},
        {"USB3-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"USB3-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"USB3-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"USB3-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
    };

    std::vector<TestResult> results;
    results.reserve(tests.size());
    for (const auto &test : tests)
    {
        try
        {
            // Run using the consolidated function with a single device path
            results.push_back(measure_capture_performance(test, {"/dev/video4"}));
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "ERROR: USB3 Test '{}' failed: {}\n", test.label, e.what());
        }
    }

    print_results(results);
    fmt::print("Testing 'USB3' camera performance done\n\n");
}

void test_usb3_dual_cam()
{
    fmt::print("Testing 'USB3' dual camera performance\n");
    // *** IMPORTANT: Ensure /dev/video0 and /dev/video2 are connected via USB3 for these tests to be meaningful ***
    std::vector<TestCase> tests = {
        {"USB3-DUAL-4K-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"USB3-DUAL-4K-30", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"USB3-DUAL-4K-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"USB3-DUAL-4K-15", v4l2::PixelDimension::DIM_4K, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4},
        {"USB3-DUAL-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 2},
        {"USB3-DUAL-FHD-30", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_30, 4},
        {"USB3-DUAL-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 2},
        {"USB3-DUAL-FHD-15", v4l2::PixelDimension::DIM_FHD, v4l2::PixelFormat::MJPG, v4l2::FPS::FPS_15, 4}};

    std::vector<std::string> dual_device_paths = {"/dev/video4", "/dev/video6"};
    std::vector<TestResult> results;
    results.reserve(tests.size());
    for (const auto &test : tests)
    {
        try
        {
            // Run using the consolidated function with the specified dual device paths
            results.push_back(measure_capture_performance(test, dual_device_paths));
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "ERROR: USB3 Dual Test '{}' failed: {}\n", test.label, e.what());
        }
    }

    print_results(results);
    fmt::print("Testing 'USB3' dual camera performance done\n\n");
}

// Function to stress USB bandwidth with a single high-load camera configuration
void stress_usb_bandwidth()
{
    fmt::print("Starting USB bandwidth stress test (using /dev/video0)\n");

    // Define a high-bandwidth configuration for the stress test
    v4l2::V4l2Config config{};
    config.device_path_ = "/dev/video0";               // *** Ensure this camera is on the bus you want to stress ***
    config.dimension_ = v4l2::PixelDimension::DIM_FHD; // Or DIM_4K if available/needed
    config.format_ = v4l2::PixelFormat::MJPG;          // Or YUYV for higher raw bandwidth
    config.fps_num_ = v4l2::FPS::FPS_30;               // Or FPS_60 if available
    config.buffer_count_ = 8;                          // Use a higher buffer count

    auto [w, h] = v4l2::dimensions_decompress(static_cast<std::uint32_t>(config.dimension_)); // Decompress for logging

    fmt::print("  Using Config: Device: {}, Res: {}, Fmt: {}, FPS: {}, Bufs: {}\n",
               config.device_path_,
               fmt::format("{}x{}", w, h), // Format here
               config.format_ == v4l2::PixelFormat::MJPG ? "MJPG" : "YUYV",
               static_cast<uint32_t>(config.fps_num_),
               config.buffer_count_);

    std::unique_ptr<v4l2::V4L2Camera> cam;
    try
    {
        cam = std::make_unique<v4l2::V4L2Camera>(config);
        cam->open_device();
        cam->configure();
        cam->start_streaming();
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "ERROR: Failed to set up camera for stress test: {}\n", e.what());
        return; // Cannot proceed
    }

    constexpr auto duration_sec = 30;
    const auto start_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    // Use unsigned type matching std::vector::size() to avoid sign conversion issues
    std::uint64_t total_bytes = 0; // Changed to unsigned 64-bit integer

    fmt::print("  Running stress test for {} seconds...\n", duration_sec);

    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(duration_sec))
    {
        try
        {
            auto const frame = cam->capture_frame();
            cam->release_frame();
            ++frame_count;
            total_bytes += frame.image.size(); // Now adding size_t to uint64_t (safe)
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "ERROR during stress capture: {}\n", e.what());
            // Consider breaking or logging frequency of errors
            break; // Stop test on error
        }
    }

    try
    {
        if (cam)
        { // Check unique_ptr is valid
            // Attempt stop_streaming without checking is_streaming()
            cam->stop_streaming();
        }
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "WARN: Error stopping camera stream after stress test: {}\n", e.what());
    }
    catch (...)
    {
        fmt::print(stderr, "WARN: Unknown error stopping camera stream after stress test.\n");
    }

    const auto end_time = std::chrono::steady_clock::now();
    const double actual_duration_sec = std::chrono::duration<double>(end_time - start_time).count();
    const double avg_fps = (actual_duration_sec > 0) ? static_cast<double>(frame_count) / actual_duration_sec : 0.0;
    const double avg_mbps = (actual_duration_sec > 0) ? (static_cast<double>(total_bytes) / 1'000'000.0) / actual_duration_sec : 0.0;

    fmt::print("  Stress Test Summary:\n");
    fmt::print("    Captured {} frames in {:.2f} seconds.\n", frame_count, actual_duration_sec);
    fmt::print("    Average FPS: {:.2f}\n", avg_fps);
    fmt::print("    Average Bandwidth: {:.2f} MB/s\n", avg_mbps);

    fmt::print("  Checking kernel logs for USB errors post-stress test...\n");
    // Refined grep pattern for potentially more relevant errors
    int ret = system("dmesg | tail -n 150 | grep -E --color=always 'usb.*(reset|error|fail|disconnect|xhci.*(died|halt|error|warn))'");
    if (ret != 0) // system returns 0 if grep finds matches
    {
        fmt::print("    No significant USB errors found in recent kernel logs.\n");
    }
    else
    {
        fmt::print("    WARN: Potential USB issues detected in recent dmesg output (see highlighted messages above).\n");
    }
    fmt::print("USB bandwidth stress test done\n\n");
}

// Main function to orchestrate the tests
int main()
{
    fmt::print("Starting V4L2 Camera Performance Tests.\n");
    fmt::print("NOTE: Stress test and kernel log checks might require elevated privileges (e.g., run with sudo or as root, or use `docker run --privileged`).\n\n");

    // Add placeholder definitions if rocx types are not fully defined
    // Ensure PixelDimension, PixelFormat, FPS enums/types are properly defined or included
    // Example: using v4l2::PixelDimension::DIM_FHD; etc.

    try
    {
        test_single_camera();
        test_dual_camera();     // Ensure /dev/video0 and /dev/video2 exist and are configured correctly
        test_usb3_cam();        // Ensure /dev/video0 is connected via USB3 for meaningful results
        test_usb3_dual_cam();   // Ensure /dev/video4 and /dev/video6 are connected via USB3 for meaningful results
        stress_usb_bandwidth(); // Requires write access potentially implied by --privileged for dmesg checks depending on system config.
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "FATAL ERROR during test execution: {}\n", e.what());
        return 1;
    }
    catch (...)
    {
        fmt::print(stderr, "FATAL UNKNOWN ERROR during test execution.\n");
        return 1;
    }

    fmt::print("All tests finished successfully.\n");
    return 0;
}
