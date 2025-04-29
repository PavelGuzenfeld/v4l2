#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Some kernels may not define these:
#ifndef V4L2_CID_TIMESTAMP_SOURCE
#define V4L2_CID_TIMESTAMP_SOURCE (V4L2_CID_USER_BASE + 0x1029)
#endif

#ifndef V4L2_TIMESTAMP_SRC_EOF
#define V4L2_TIMESTAMP_SRC_EOF 0
#define V4L2_TIMESTAMP_SRC_SOE 1
#endif

// Buffer struct for our MMAP
struct MmapBuffer
{
    void *start = nullptr;
    size_t length = 0;
};

int main()
{
    // Camera device path
    const char *devName = "/dev/video0";

    // 1. Open the device
    int fd = open(devName, O_RDWR);
    if (fd < 0)
    {
        std::cerr << "Error opening " << devName << ": " << strerror(errno) << std::endl;
        return 1;
    }

    // 2. Query device capabilities (just to confirm it's a valid V4L2 capture device)
    {
        v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
        {
            std::cerr << "VIDIOC_QUERYCAP error: " << strerror(errno) << std::endl;
            close(fd);
            return 1;
        }

        std::cout << "Driver:      " << cap.driver << "\n"
                  << "Card:        " << cap.card << "\n"
                  << "Bus:         " << cap.bus_info << "\n"
                  << "Version:     "
                  << ((cap.version >> 16) & 0xFF) << "."
                  << ((cap.version >> 8) & 0xFF) << "."
                  << (cap.version & 0xFF) << "\n\n";

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {
            std::cerr << "This device does not support VIDEO_CAPTURE.\n";
            close(fd);
            return 1;
        }
    }

    // 3. Attempt to set Timestamp Source to SOE (if supported)
    {
        v4l2_queryctrl qctrl;
        memset(&qctrl, 0, sizeof(qctrl));
        qctrl.id = V4L2_CID_TIMESTAMP_SOURCE;

        if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0)
        {
            // Driver supports changing timestamp source
            v4l2_control ctrl;
            memset(&ctrl, 0, sizeof(ctrl));
            ctrl.id = V4L2_CID_TIMESTAMP_SOURCE;
            ctrl.value = V4L2_TIMESTAMP_SRC_SOE;

            if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0)
            {
                std::cout << "[INFO] Successfully set timestamp source to START-OF-EXPOSURE.\n";
            }
            else
            {
                std::cerr << "[WARN] Failed to set SOE timestamp source: "
                          << strerror(errno) << "\n";
            }
        }
        else
        {
            std::cerr << "[WARN] Driver does not support V4L2_CID_TIMESTAMP_SOURCE.\n"
                      << "       Timestamps may be end-of-frame or real-time.\n";
        }
    }

    // 4. Set capture format to 4K (3840x2160) MJPEG
    {
        v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 3840;  // 4K width
        fmt.fmt.pix.height = 2160; // 4K height
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        {
            std::cerr << "VIDIOC_S_FMT error: " << strerror(errno) << "\n";
            close(fd);
            return 1;
        }
        std::cout << "[INFO] Set format to 3840x2160 MJPEG.\n";
    }

    // 5. Try setting the frame rate to 30 fps via VIDIOC_S_PARM
    {
        v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        // time per frame = numerator/denominator
        // For 30 fps, timeperframe = 1/30
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 30;

        if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0)
        {
            std::cerr << "[WARN] VIDIOC_S_PARM (frame rate) failed: "
                      << strerror(errno) << "\n";
        }
        else
        {
            std::cout << "[INFO] Requested 30 fps.\n";
        }
    }

    // 6. Request and mmap buffers
    const unsigned int NUM_BUFFERS = 4;
    {
        v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = NUM_BUFFERS;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        {
            std::cerr << "VIDIOC_REQBUFS error: " << strerror(errno) << "\n";
            close(fd);
            return 1;
        }
        if (req.count < NUM_BUFFERS)
        {
            std::cerr << "[WARN] Requested " << NUM_BUFFERS
                      << " buffers, but got " << req.count << "\n";
        }
    }

    MmapBuffer *buffers = new MmapBuffer[NUM_BUFFERS];
    for (unsigned int i = 0; i < NUM_BUFFERS; i++)
    {
        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            std::cerr << "VIDIOC_QUERYBUF error: " << strerror(errno) << "\n";
            delete[] buffers;
            close(fd);
            return 1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(nullptr, buf.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED)
        {
            std::cerr << "mmap error: " << strerror(errno) << "\n";
            delete[] buffers;
            close(fd);
            return 1;
        }
    }

    // 7. Queue buffers
    for (unsigned int i = 0; i < NUM_BUFFERS; i++)
    {
        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            std::cerr << "VIDIOC_QBUF error: " << strerror(errno) << "\n";
            // cleanup
            for (unsigned int j = 0; j < NUM_BUFFERS; j++)
            {
                if (buffers[j].start && buffers[j].start != MAP_FAILED)
                {
                    munmap(buffers[j].start, buffers[j].length);
                }
            }
            delete[] buffers;
            close(fd);
            return 1;
        }
    }

    // 8. Start streaming
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
        {
            std::cerr << "VIDIOC_STREAMON error: " << strerror(errno) << "\n";
            // cleanup
            for (unsigned int i = 0; i < NUM_BUFFERS; i++)
            {
                if (buffers[i].start && buffers[i].start != MAP_FAILED)
                {
                    munmap(buffers[i].start, buffers[i].length);
                }
            }
            delete[] buffers;
            close(fd);
            return 1;
        }
        std::cout << "[INFO] Streaming started at (expected) 4K 30 fps MJPEG.\n";
    }

    // 9. Capture frames and compare timestamps to monotonic clock
    const unsigned int NUM_FRAMES_TO_CAPTURE = 10;
    for (unsigned int frameIdx = 0; frameIdx < NUM_FRAMES_TO_CAPTURE; frameIdx++)
    {
        // Dequeue
        v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        {
            std::cerr << "VIDIOC_DQBUF error: " << strerror(errno) << "\n";
            break;
        }

        // Check if the driver claims SOE
        bool isSOE = (buf.flags & V4L2_BUF_FLAG_TSTAMP_SRC_SOE);

        // Driver-provided timestamp
        double drvSec = static_cast<double>(buf.timestamp.tv_sec) + static_cast<double>(buf.timestamp.tv_usec) / 1e6;

        // Current system monotonic time
        struct timespec tsNow;
        clock_gettime(CLOCK_MONOTONIC, &tsNow);
        double sysSec = static_cast<double>(tsNow.tv_sec) + static_cast<double>(tsNow.tv_nsec) / 1e9;

        double offsetSec = sysSec - drvSec; // offset in seconds
        double offsetMs = offsetSec * 1000; // offset in milliseconds

        std::cout << "[Frame " << frameIdx << "]  "
                  << "Driver TS=" << drvSec << " s "
                  << (isSOE ? "(START-OF-EXPOSURE), " : "(EOF or unknown), ")
                  << "SysMonotonic=" << sysSec << " s, "
                  << "Offset=" << offsetMs << " ms\n";

        // (Optionally) process the MJPEG data at buffers[buf.index].start (buf.bytesused bytes)
        // ...
        // Re-queue the buffer
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            std::cerr << "VIDIOC_QBUF (re-queue) error: " << strerror(errno) << "\n";
            break;
        }
    }

    // 10. Stop streaming and clean up
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
        {
            std::cerr << "VIDIOC_STREAMOFF error: " << strerror(errno) << "\n";
        }
    }

    for (unsigned int i = 0; i < NUM_BUFFERS; i++)
    {
        if (buffers[i].start && buffers[i].start != MAP_FAILED)
        {
            munmap(buffers[i].start, buffers[i].length);
        }
    }
    delete[] buffers;
    close(fd);

    std::cout << "[INFO] Capture complete. Exiting.\n";
    return 0;
}