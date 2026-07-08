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
    EXPECT_THROW((void)Loader::load_ihex(mem, "/nonexistent/file.hex"), LoaderError);
}

TEST_F(LoaderTest, LoadHexEofRecord) {
    std::string hex = ":00000001FF\n";
    auto path = write_file("eof.hex", hex);
    // Should not throw; entry = 0
    uint32_t entry = Loader::load_ihex(mem, path.string());
    EXPECT_EQ(entry, 0u);
}

// ─── Additional coverage: HEX record types & error paths ─────────────────────

TEST_F(LoaderTest, InvalidHexByteThrows) {
    // parse_hex_byte with invalid chars → LoaderError
    // Use a record with garbage hex in data bytes
    std::string hex =
        ":01000000ZZ\n"  // 'ZZ' is invalid hex
        ":00000001FF\n";
    auto path = write_file("bad_hex.hex", hex);
    EXPECT_THROW((void)Loader::load_ihex(mem, path.string()), LoaderError);
}

TEST_F(LoaderTest, BadChecksumThrows) {
    // Valid format but wrong checksum (last byte)
    std::string hex =
        ":04010000DEADBEEF00\n"  // checksum=00 instead of correct value
        ":00000001FF\n";
    auto path = write_file("bad_cksum.hex", hex);
    EXPECT_THROW((void)Loader::load_ihex(mem, path.string()), LoaderError);
}

TEST_F(LoaderTest, ShortHexRecordThrows) {
    // Record shorter than 11 chars minimum
    std::string hex = ":01\n";
    auto path = write_file("short.hex", hex);
    EXPECT_THROW((void)Loader::load_ihex(mem, path.string()), LoaderError);
}

TEST_F(LoaderTest, UnknownRecordTypeThrows) {
    // Record type 0x03 is not supported
    // :01 0000 03 AA checksum
    // sum = 01+00+00+03+AA = AE → checksum = 0x100-0xAE = 0x52
    std::string hex = ":0100000352" "\n";
    auto path = write_file("unk.hex", hex);
    EXPECT_THROW((void)Loader::load_ihex(mem, path.string()), LoaderError);
}

TEST_F(LoaderTest, ExtendedSegmentAddress) {
    // Type 0x02: extended segment address shifts base by <<4
    // :02 0000 02 0010 EC   → base = 0x0010 << 4 = 0x100
    // Then data record at offset 0x0000 lands at 0x100
    // Checksum for :020000020010: sum=02+00+00+02+00+10=14 → ~14+1=EC ✓
    std::string hex =
        ":020000020010EC\n"
        ":0100000001FE\n"  // 1 byte, addr=0x0000, data=0x01, cksum=FE
        ":00000001FF\n";
    auto path = write_file("seg.hex", hex);
    (void)Loader::load_ihex(mem, path.string());
    EXPECT_EQ(mem.read8(0x100), 0x01);
}

TEST_F(LoaderTest, ExtendedLinearAddress) {
    // Type 0x04: extended linear address → base = val<<16
    // We'll use base=0x0000 (no-op) then data at low address
    // :02 0000 04 0000 FA  → base=0x00000000
    // sum=02+00+00+04+00+00=06 → cksum=FA ✓
    std::string hex =
        ":020000040000FA\n"
        ":0100000042BD\n"  // 1 byte at 0x0000+0x0000=0x0000, data=0x42; cksum=0x100-0x43=0xBD
        ":00000001FF\n";
    auto path = write_file("lin.hex", hex);
    (void)Loader::load_ihex(mem, path.string());
    EXPECT_EQ(mem.read8(0x0000), 0x42);
}

TEST_F(LoaderTest, StartLinearAddressReturnsEntryPoint) {
    // Type 0x05: 4-byte entry point
    // :04 0000 05 00000200 F5
    // sum=04+00+00+05+00+00+02+00=0B → cksum=F5 ✓
    std::string hex =
        ":0400000500000200F5\n"
        ":00000001FF\n";
    auto path = write_file("entry.hex", hex);
    uint32_t entry = Loader::load_ihex(mem, path.string());
    EXPECT_EQ(entry, 0x00000200u);
}

TEST_F(LoaderTest, EmptyBinaryThrows) {
    auto path = tmp_dir / "empty.bin";
    { std::ofstream f(path); } // create empty file
    EXPECT_THROW((void)Loader::load_binary(mem, path.string(), 0), LoaderError);
}

TEST_F(LoaderTest, HexSkipsNonColonLines) {
    // Lines not starting with ':' should be silently skipped
    std::string hex =
        "; This is a comment\n"
        ":04010000DEADBEEFC3\n"
        ":00000001FF\n";
    auto path = write_file("comment.hex", hex);
    EXPECT_NO_THROW((void)Loader::load_ihex(mem, path.string()));
    EXPECT_EQ(mem.read8(0x100), 0xDE);
}
