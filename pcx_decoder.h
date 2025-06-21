#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <fstream>

struct PCXImage {
    int width;
    int height;
    std::vector<uint8_t> rgba_data; // Now includes alpha
};

inline PCXImage load_pcx_from_memory(const std::vector<uint8_t>& data) {
    if (data.size() < 128 + 769)
        throw std::runtime_error("Data too small to be a valid PCX");

    const uint8_t* header = data.data();
    if (header[0] != 0x0A || header[1] != 5 || header[2] != 1 || header[3] != 8)
        throw std::runtime_error("Unsupported PCX format (only 8-bit)");

    int xmin = header[4] | (header[5] << 8);
    int ymin = header[6] | (header[7] << 8);
    int xmax = header[8] | (header[9] << 8);
    int ymax = header[10] | (header[11] << 8);
    int width = xmax - xmin + 1;
    int height = ymax - ymin + 1;

    int bytes_per_line = header[66] | (header[67] << 8);
    std::vector<uint8_t> image_data(width * height);

    size_t pos = 128;
    for (int y = 0; y < height; ++y) {
        std::vector<uint8_t> scanline(bytes_per_line);
        size_t x = 0;
        while (x < bytes_per_line && pos < data.size() - 769) {
            uint8_t c = data[pos++];
            if ((c & 0xC0) == 0xC0) {
                int count = c & 0x3F;
                if (pos >= data.size() - 769)
                    throw std::runtime_error("Unexpected end of data");
                uint8_t val = data[pos++];
                std::fill_n(scanline.begin() + x, count, val);
                x += count;
            } else {
                scanline[x++] = c;
            }
        }
        std::copy(scanline.begin(), scanline.begin() + width, image_data.begin() + y * width);
    }

    // Load palette
    size_t palette_start = data.size() - 769;
    if (data[palette_start] != 0x0C)
        throw std::runtime_error("Missing palette marker");
    const uint8_t* palette = &data[palette_start + 1];

    // Determine which palette index is #00FF00
    std::vector<bool> transparent_index(256, false);
    for (int i = 0; i < 256; ++i) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        if (r == 0x00 && g == 0xFF && b == 0x00)
            transparent_index[i] = true;
    }

    // Convert indexed image to RGBA
    std::vector<uint8_t> rgba(width * height * 4);
    for (int i = 0; i < width * height; ++i) {
        int idx = image_data[i];
        rgba[i * 4 + 0] = palette[idx * 3 + 0];             // R
        rgba[i * 4 + 1] = palette[idx * 3 + 1];             // G
        rgba[i * 4 + 2] = palette[idx * 3 + 2];             // B
        rgba[i * 4 + 3] = transparent_index[idx] ? 0 : 255; // A
    }

    return {width, height, std::move(rgba)};
}
