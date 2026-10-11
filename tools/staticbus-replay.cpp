// SPDX-License-Identifier: GPL-2.0-or-later
// Replays a binary capture file into the /dev/staticbus0 writer API.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " DEVICE CAPTURE.bin\n"
                  << "Example: sudo " << argv[0]
                  << " /dev/staticbus0 samples.bin\n";
        return 2;
    }

    const int input = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        std::cerr << "Cannot open capture " << argv[2] << ": " << std::strerror(errno) << '\n';
        return 1;
    }
    const int output = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (output < 0) {
        std::cerr << "Cannot open device " << argv[1] << ": " << std::strerror(errno) << '\n';
        close(input);
        return 1;
    }

    std::vector<unsigned char> buffer(4096);
    unsigned long long total = 0;
    for (;;) {
        const ssize_t got = read(input, buffer.data(), buffer.size());
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Capture read failed: " << std::strerror(errno) << '\n';
            close(input); close(output); return 1;
        }
        ssize_t offset = 0;
        while (offset < got) {
            const ssize_t sent = write(output, buffer.data() + offset,
                                       static_cast<std::size_t>(got - offset));
            if (sent < 0) {
                if (errno == EINTR) continue;
                std::cerr << "Device write failed: " << std::strerror(errno) << '\n';
                close(input); close(output); return 1;
            }
            offset += sent;
            total += static_cast<unsigned long long>(sent);
        }
    }

    close(input);
    close(output);
    std::cout << "Replayed " << total << " bytes into " << argv[1] << '\n';
    return 0;
}
