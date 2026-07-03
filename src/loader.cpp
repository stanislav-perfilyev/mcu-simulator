#include "loader.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <charconv>
#include <cstring>

uint8_t Loader::parse_hex_byte(const char* s) {
    uint8_t val = 0;
    auto result = std::from_chars(s, s + 2, val, 16);
    if (result.ec != std::errc{})
        throw LoaderError(std::string("Invalid hex byte: ") + s[0] + s[1]);
    return val;
}

void Loader::verify_checksum(const std::string& line) {
    // line starts with ':'
    auto count = static_cast<int>((line.size() - 1) / 2);
    uint8_t sum = 0;
    for (int i = 0; i < count; ++i)
        sum += parse_hex_byte(line.c_str() + 1 + i * 2);
    if (sum != 0)
        throw LoaderError("Checksum error in HEX record: " + line);
}

uint32_t Loader::load_binary(IMemoryBus& mem, const std::string& path, uint32_t load_addr) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw LoaderError("Cannot open binary: " + path);

    auto size = file.tellg();
    if (size <= 0) throw LoaderError("Empty binary: " + path);

    file.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buf.data()), size))
        throw LoaderError("Read error: " + path);

    mem.load(load_addr, buf.data(), buf.size());
    return load_addr; // binary starts at load address
}

uint32_t Loader::load_ihex(IMemoryBus& mem, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw LoaderError("Cannot open HEX file: " + path);

    uint32_t base_addr   = 0;
    uint32_t entry_point = 0;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] != ':') continue;
        if (line.size() < 11) throw LoaderError("Short HEX record: " + line);

        // Remove trailing CR/LF
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        verify_checksum(line);

        uint8_t byte_count = parse_hex_byte(line.c_str() + 1);
        uint16_t addr_hi   = parse_hex_byte(line.c_str() + 3);
        uint16_t addr_lo   = parse_hex_byte(line.c_str() + 5);
        uint16_t offset    = static_cast<uint16_t>((addr_hi << 8) | addr_lo);
        uint8_t  rec_type  = parse_hex_byte(line.c_str() + 7);

        switch (rec_type) {
            case 0x00: { // Data
                std::vector<uint8_t> data(byte_count);
                for (int i = 0; i < byte_count; ++i)
                    data[static_cast<size_t>(i)] = parse_hex_byte(line.c_str() + 9 + i * 2);
                mem.load(base_addr + offset, data.data(), data.size());
                break;
            }
            case 0x01: // EOF
                return entry_point;
            case 0x02: { // Extended segment address
                uint8_t hi = parse_hex_byte(line.c_str() + 9);
                uint8_t lo = parse_hex_byte(line.c_str() + 11);
                base_addr  = static_cast<uint32_t>((hi << 8 | lo)) << 4;
                break;
            }
            case 0x04: { // Extended linear address
                uint8_t hi = parse_hex_byte(line.c_str() + 9);
                uint8_t lo = parse_hex_byte(line.c_str() + 11);
                base_addr  = static_cast<uint32_t>((hi << 8 | lo)) << 16;
                break;
            }
            case 0x05: { // Start linear address (entry point)
                for (int i = 0; i < 4; ++i)
                    entry_point = (entry_point << 8) | parse_hex_byte(line.c_str() + 9 + i * 2);
                break;
            }
            default:
                throw LoaderError("Unknown HEX record type: " + std::to_string(rec_type));
        }
    }
    return entry_point;
}
