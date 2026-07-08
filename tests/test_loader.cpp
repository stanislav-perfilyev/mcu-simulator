#include <gtest/gtest.h>
#include "loader.h"
#include "memory.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class LoaderTest : public ::testing::Test {
protected:
    FlatMemoryBus mem;
    fs::path      tmp_dir;

    void SetUp() override {
        // Unique dir per test to avoid race conditions under parallel ctest -j
        const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string suffix = std::string(ti->test_suite_name()) + "_" + ti->name();
        tmp_dir = fs::temp_directory_path() / ("mcu_loader_" + suffix);
        fs::create_directories(tmp_dir);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }

    fs::path write_file(const std::string& name, const std::string& content) {
        auto path = tmp_dir / name;
        std::ofstream f(path);
        f << content;
        return path;
    }

    fs::path write_binary(const std::string& name, const std::vector<uint8_t>& bytes) {
        auto path = tmp_dir / name;
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};

// ─── Binary loader ────────────────────────────────────────────────────────────

TEST_F(LoaderTest, LoadBinaryAtOffset0) {
    auto path = write_binary("prog.bin", {0x01, 0x02, 0x03, 0x04});
    uint32_t entry = Loader::load_binary(mem, path.string(), 0);
    EXPECT_EQ(entry, 0u);
    EXPECT_EQ(mem.read8(0), 0x01);
    EXPECT_EQ(mem.read8(3), 0x04);
}

TEST_F(LoaderTest, LoadBinaryAtCustomAddr) {
    auto path = write_binary("prog.bin", {0xAB, 0xCD});
    (void)Loader::load_binary(mem, path.string(), 0x200);
    EXPECT_EQ(mem.read8(0x200), 0xAB);
    EXPECT_EQ(mem.read8(0x201), 0xCD);
    EXPECT_EQ(mem.read8(0x000), 0x00); // not written
}

TEST_F(LoaderTest, LoadBinaryMissingFileThrows) {
    EXPECT_THROW((void)Loader::load_binary(mem, "/nonexistent/file.bin", 0), LoaderError);
}

// ─── Intel HEX loader ────────────────────────────────────────────────────────

TEST_F(LoaderTest, LoadHexSimple) {
    // Standard Intel HEX record with 4 data bytes at address 0x0100
    // :04010000DEADBEEFC3
    // byte_count=04, addr=0100, type=00, data=DEADBEEF, checksum=78
    // Checksum: 0x100 - (04+01+00+00+DE+AD+BE+EF) & 0xFF = 0x100-0x288 = 0x78 ✓
    std::string hex =
        ":04010000DEADBEEFC3\n"
        ":00000001FF\n";
    auto path = write_file("prog.hex", hex);
    (void)Loader::load_ihex(mem, path.string());
    EXPECT_EQ(mem.read8(0x100), 0xDE);
    EXPECT_EQ(mem.read8(0x101), 0xAD);
    EXPECT_EQ(mem.read8(0x102), 0xBE);
    EXPECT_EQ(mem.read8(0x103), 0xEF);
}

TEST_F(LoaderTest, LoadHexMissingFileThrows) {
    EXPECT_THROW((void)Loader::load_ihex(mem, "/nonexistent/file.hex"), LoaderErr