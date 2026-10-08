#include "animation_tests.h"
#include "animation_candidate.h"
#include "animation_validation.h"
#include "byte_storage.h"
#include "general_resolver_runtime_tests.h"
#include "loader_config.h"
#include "mod_index.h"
#include "sha256.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace ds2::modding;
void Require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Write(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(out.good(), "animation fixture write failed");
}

// Records, independently of the validator, which bits a keys_in_place edit may change.
class Writer final {
public:
    AnimationFixture fixture;
    void U8(const unsigned value, const std::uint8_t bits = 0) {
        fixture.bytes.push_back(static_cast<std::byte>(value & 0xFFu));
        fixture.mutable_bits.push_back(bits);
    }
    void U16(const unsigned value, const std::uint8_t bits = 0) { U8(value, bits); U8(value >> 8, bits); }
    void U32(const std::uint32_t value) { U16(value & 0xFFFFu); U16(value >> 16); }
    void U64(const std::uint64_t value) {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32));
    }
    void Align(const std::size_t size) { while (fixture.bytes.size() % size != 0) U8(0); }
    void String(const std::string_view text) {
        for (const char c : text) U8(static_cast<unsigned char>(c));
        U8(0);
    }
    void Times(const std::vector<unsigned>& deltas) {
        for (std::size_t i = 0; i < deltas.size(); ++i) {
            if (i > 0) fixture.later_times.push_back(Size());
            U8(deltas[i]);
        }
        Align(2);
    }
    void Residuals(const std::vector<std::array<int, 3>>& residuals) {
        for (const auto& row : residuals)
            for (const int value : row) U8(static_cast<unsigned>(value), 0xFF);
    }
    [[nodiscard]] std::size_t Size() const noexcept { return fixture.bytes.size(); }
};

struct RotationSegment final {
    std::array<int, 3> base;
    std::array<unsigned, 3> step;
    unsigned omitted, run;
};
void Rotation(Writer& w, const std::vector<RotationSegment>& segments,
              const std::vector<std::array<int, 3>>& residuals, const std::vector<unsigned>& deltas) {
    w.U16(static_cast<unsigned>(deltas.size()));
    if (deltas.empty()) return;
    for (const auto& segment : segments) {
        for (std::size_t c = 0; c < 3; ++c)
            w.U8((static_cast<unsigned>(segment.base[c]) & 63u) | (segment.step[c] << 6), 0xFF);
        w.U8((segment.omitted << 6) | (segment.run - 1), 0xC0);
    }
    w.Residuals(residuals);
    w.Times(deltas);
}
void Position(Writer& w, const std::vector<std::pair<unsigned, std::array<int, 3>>>& segments,
              const std::vector<std::array<int, 3>>& residuals, const std::vector<unsigned>& deltas) {
    w.U16(static_cast<unsigned>(deltas.size()));
    for (const auto& [run, base] : segments) {
        w.U16(run);
        for (const int value : base) w.U16(static_cast<unsigned>(value), 0xFF);
    }
    w.Residuals(residuals);
    w.Times(deltas);
}
void Scale(Writer& w, const std::vector<unsigned>& halves, const std::vector<unsigned>& deltas) {
    w.U16(static_cast<unsigned>(deltas.size()));
    w.fixture.scale_values = w.Size();
    for (const auto half : halves) w.U16(half, 0xFF);
    w.Times(deltas);
}
void Scalar(Writer& w, const std::vector<unsigned>& values, const std::vector<unsigned>& deltas) {
    w.U16(static_cast<unsigned>(deltas.size()));
    for (const auto value : values) w.U8(value, 0xFF);
    w.Times(deltas);
}
void HashedTrack(Writer& w, const unsigned flags, const std::uint64_t hash, const unsigned weight) {
    w.U8(flags);
    w.Align(8);
    w.U64(hash);
    w.Align(2);
    w.U16(weight);
}

bool Contains(const std::wstring& text, const std::wstring_view needle) {
    return text.find(needle) != std::wstring::npos;
}
std::vector<std::byte> WithSize(std::vector<std::byte> bytes) {
    const auto size = static_cast<std::uint32_t>(bytes.size() - 16);
    for (std::size_t i = 0; i < 4; ++i) bytes[8 + i] = static_cast<std::byte>((size >> (8 * i)) & 0xFFu);
    return bytes;
}
}

AnimationFixture MakeTestAnimation() {
    Writer w;
    for (const unsigned c : {'A', 'N', 'M'}) w.U8(c);
    w.U8(1); w.U8(0); w.U8(30); w.U16(20); w.U32(0); w.U32(1);  // v1, LE, 30 FPS, 20 frames, size, loop
    w.String("fixture_clip");
    // Inline name, +128 rotation offset, two interpolated rotation segments.
    w.U8(1 | 2 | 32); w.String("root"); w.Align(2); w.U16(0x7FFF);
    Rotation(w, {{{10, -5, 3}, {0, 1, 2}, 3, 2}, {{12, -6, 4}, {1, 0, 3}, 3, 2}},
             {{1, -2, 3}, {4, 5, -6}, {7, -8, 9}, {-1, 2, -3}}, {0, 4, 4, 8});
    Position(w, {{2u, {100, -40, 7}}}, {{1, 2, 3}, {-4, -5, -6}}, {0, 19});
    w.fixture.track_ends.push_back(w.Size());
    // Hashed name aligned to 8 with position, scale and scalar channels.
    HashedTrack(w, 2 | 4 | 8 | 32, 0x0123456789ABCDEFull, 0x4000);
    Rotation(w, {{{-3, 2, 1}, {0, 0, 0}, 0, 1}}, {{0, 0, 0}}, {0});
    Position(w, {{1u, {5, 6, 7}}, {2u, {8, 9, 10}}}, {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}, {0, 10, 10});
    Scale(w, {0x3C00, 0x3C00, 0x3C00, 0x3800, 0x3C00, 0x4000}, {0, 20});
    Scalar(w, {0, 128, 255}, {0, 5, 5});
    w.fixture.track_ends.push_back(w.Size());
    // Fine position quantization.
    HashedTrack(w, 2 | 16 | 32, 0xFEDCBA9876543210ull, 1);
    Rotation(w, {{{0, 0, 0}, {3, 3, 3}, 1, 1}}, {{0, 0, 0}}, {0});
    Position(w, {{1u, {-1, -2, -3}}}, {{127, -128, 0}}, {0});
    w.fixture.track_ends.push_back(w.Size());
    // Rotation only, without the +128 offset: the unit-sphere probe.
    HashedTrack(w, 0, 0x1111, 2);
    w.fixture.unit_residuals = w.Size() + 2 + 4;
    Rotation(w, {{{1, 0, 0}, {0, 0, 0}, 3, 1}}, {{0, 0, 0}}, {0});
    w.fixture.track_ends.push_back(w.Size());
    // Empty rotation curve followed by a single position key.
    HashedTrack(w, 2, 0x2222, 3);
    Rotation(w, {}, {}, {});
    Position(w, {{1u, {0, 0, 0}}}, {{0, 0, 0}}, {0});
    w.fixture.track_ends.push_back(w.Size());
    w.fixture.bytes = WithSize(std::move(w.fixture.bytes));
    return w.fixture;
}

void TestAnimations(const std::filesystem::path& temporary_root) {
    const auto fixture = MakeTestAnimation();
    const auto& original = fixture.bytes;
    const auto parsed = ParseAnimation(original);
    Require(bool(parsed) && parsed.layout.tracks == 5 && parsed.layout.keys == 19 &&
            parsed.layout.fps == 30 && parsed.layout.frame_count == 20 && parsed.layout.flags == 1,
            "synthetic animation layout rejected");
    const auto unchanged = ValidateAnimationEdit(original, original);
    Require(bool(unchanged) && unchanged.changed_tracks == 0, "unchanged animation rejected");

    // Every single-bit change: structure must never pass; values pass unless
    // they leave the decoder's range.
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto edited = original;
            edited[i] ^= static_cast<std::byte>(1u << bit);
            const auto result = ValidateAnimationEdit(original, edited);
            if (((fixture.mutable_bits[i] >> bit) & 1u) == 0) {
                Require(!result, "immutable animation bit change accepted");
            } else if (result) {
                Require(result.changed_tracks == 1, "value edit not attributed to one track");
                ++accepted;
            } else {
                Require(Contains(result.error, L"quaternion") || Contains(result.error, L"scale") ||
                        Contains(result.error, L"unit sphere"), "value edit rejected as structural");
            }
        }
    }
    Require(accepted > 200, "too few value edits accepted");

    auto invalid = original; invalid.push_back(std::byte{});
    Require(!ValidateAnimationEdit(original, invalid), "resized animation accepted");
    Require(!ParseAnimation(WithSize(invalid)), "trailing animation bytes accepted");
    for (std::size_t cut = 0; cut < original.size(); ++cut) {
        std::vector<std::byte> truncated(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(cut));
        Require(!ParseAnimation(truncated), "truncated animation accepted");
        if (cut <= 16) continue;
        const bool boundary = std::find(fixture.track_ends.begin(), fixture.track_ends.end(), cut) !=
            fixture.track_ends.end();
        Require(bool(ParseAnimation(WithSize(truncated))) == boundary, "animation truncation not detected");
    }
    // Magic, version, endianness, FPS, duration, header flags, first track flags.
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, unsigned>, 7>{{
             {0, 'B'}, {3, 2}, {4, 1}, {5, 0}, {6, 0}, {12, 4}, {29, 64 | 35}}}) {
        invalid = original; invalid[offset] = static_cast<std::byte>(value);
        Require(!ParseAnimation(invalid), "invalid animation header/track flags accepted");
    }
    for (const auto offset : fixture.later_times) {
        invalid = original; invalid[offset] = std::byte{};
        Require(!ParseAnimation(invalid), "repeated animation key time accepted");
    }
    invalid = original;
    for (std::size_t c = 0; c < 3; ++c) invalid[fixture.unit_residuals - 4 + c] = std::byte{0x20};
    Require(!ParseAnimation(invalid), "out-of-range compressed quaternion accepted");
    for (const unsigned half : {0x7C00u, 0xFC00u, 0x7E00u}) {
        invalid = original;
        invalid[fixture.scale_values] = static_cast<std::byte>(half & 0xFFu);
        invalid[fixture.scale_values + 1] = static_cast<std::byte>(half >> 8);
        Require(!ParseAnimation(invalid), "non-finite animation scale accepted");
    }
    // Components 26 * 256 + 32 = 6688 give a float32 squared sum of 1.0000229:
    // decodable, but an edit may not introduce it. 6676 gives 0.9964.
    auto outside = original;
    for (std::size_t c = 0; c < 3; ++c) {
        outside[fixture.unit_residuals - 4 + c] = std::byte{26};
        outside[fixture.unit_residuals + c] = std::byte{32};
    }
    Require(bool(ParseAnimation(outside)), "decoder-range rotation rejected");
    const auto sphere = ValidateAnimationEdit(original, outside);
    Require(!sphere && Contains(sphere.error, L"unit sphere"), "edit outside the unit sphere accepted");
    auto inside = outside;
    for (std::size_t c = 0; c < 3; ++c) inside[fixture.unit_residuals + c] = std::byte{20};
    Require(bool(ValidateAnimationEdit(original, inside)), "unit rotation edit rejected");
    Require(bool(ValidateAnimationEdit(outside, outside)), "unchanged decoder-range rotation rejected");

    // Opt-in indexing and the package join contract.
    auto edited = original;
    edited[fixture.unit_residuals] = std::byte{7};
    const auto mods_path = temporary_root / L"animation-mods";
    Write(mods_path / L"anim_demo/media/characters/death/fixture.anm", edited);
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto disabled = BuildModIndex(mods_path, options);
    Require(disabled && disabled.snapshot->Assets().empty(), "animation indexing not opt-in");
    options.include_animations = true;
    const auto mods = BuildModIndex(mods_path, options);
    Require(mods && mods.snapshot->Assets().size() == 1, "animation index filtering failed");
    const auto& asset = mods.snapshot->Assets().front();
    PackageIdentityEntry entry;
    entry.identity = {0x123400, 0x40, 224};
    entry.virtual_path = asset.virtual_path;
    entry.type_id = 8;
    entry.original_size = static_cast<std::uint32_t>(original.size());
    AnimationCandidate candidate; std::wstring error;
    Require(PrepareAnimationCandidate(entry, asset, original, candidate, error), "animation candidate rejected");
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    TestAnimationRuntime(candidate, original);
#endif
    Require(candidate.ranges.size() == 3 && candidate.changed_tracks == 1 && candidate.layout.tracks == 5,
            "animation candidate layout/ranges wrong");
    Require(candidate.ranges[0].original_hash != candidate.ranges[0].replacement_hash &&
            candidate.ranges[1].size == 16 &&
            candidate.ranges[1].original_hash == candidate.ranges[1].replacement_hash &&
            candidate.ranges[2].original_hash != candidate.ranges[2].replacement_hash,
            "animation range hashes wrong");
    for (const auto& range : candidate.ranges)
        Require(MatchAnimationRange(candidate, range.size, range.original_hash) == &range,
                "animation range matching failed");
    auto ambiguous = candidate; ambiguous.ranges.push_back(candidate.ranges.back());
    Require(!MatchAnimationRange(ambiguous, candidate.ranges.back().size, candidate.ranges.back().original_hash),
            "ambiguous animation range accepted");
    AnimationCandidateSnapshot snapshot;
    snapshot.entries.push_back(candidate);
    Require(snapshot.Find(entry.identity) == &snapshot.entries.front() &&
            !snapshot.Find({0x123400, 0x40, 225}), "animation snapshot lookup failed");
    auto structural = original; structural[fixture.later_times.front()] = std::byte{9};
    Require(!PrepareAnimationCandidate(entry, asset, structural, candidate, error) && !error.empty(),
            "animation with a different structure became a candidate");
    auto wrong = entry; wrong.type_id = 2;
    Require(!PrepareAnimationCandidate(wrong, asset, original, candidate, error), "wrong animation type accepted");
    wrong = entry; ++wrong.original_size;
    Require(!PrepareAnimationCandidate(wrong, asset, original, candidate, error), "animation size mismatch accepted");

    Require(ParseLoaderConfig("[loader]").config.animations == AnimationMode::off, "animations default on");
    const std::array<std::pair<const char*, AnimationMode>, 3> modes{{
        {"off", AnimationMode::off}, {"observe", AnimationMode::observe},
        {"override_keys", AnimationMode::override_keys}}};
    for (const auto& [value, mode] : modes) {
        const auto cfg = ParseLoaderConfig(std::string("[loader]\nanimations=") + value);
        Require(cfg.valid && cfg.config.animations == mode, "animation mode rejected");
    }
    const auto observe = ParseLoaderConfig("[loader]\nmode=observe\nanimations=override_keys");
    Require(observe.valid && !observe.config.write_enabled, "animation global observation lost");
    for (const auto* value : {"animations=on", "animations=override", "animations=override_shape",
                              "animations=off\nanimations=observe"})
        Require(!ParseLoaderConfig(std::string("[loader]\n") + value).valid, "invalid animation config accepted");
    std::cout << "animation_validation: PASS (keys_in_place, per-bit structure, truncation, header, times, "
                 "quaternion range, unit sphere, scale, ranges, opt-in)\n";
}

int CheckAnimationFiles(const std::filesystem::path& original, const std::filesystem::path& modified) {
    const auto before = LoadByteStorage(original, 64ull * 1024 * 1024);
    const auto after = LoadByteStorage(modified, 64ull * 1024 * 1024);
    if (!before || !after) {
        std::wcerr << L"ANIM_CHECK_FAILED: cannot read files: " << (!before ? before.detail : after.detail) << L'\n';
        return 1;
    }
    const auto result = ValidateAnimationEdit(before.storage->Bytes(), after.storage->Bytes());
    if (!result) { std::wcerr << L"ANIM_REJECTED: " << result.error << L'\n'; return 1; }
    std::wcout << L"ANIM_CHECK_PASS contract=keys_in_place bytes=" << before.storage->Size()
               << L" tracks=" << result.layout.tracks << L" keys=" << result.layout.keys
               << L" changed_tracks=" << result.changed_tracks
               << L" changed=" << (before.storage->Sha256() != after.storage->Sha256())
               << L"\noriginal_sha256=" << before.storage->Sha256Hex()
               << L"\nmodified_sha256=" << after.storage->Sha256Hex() << L'\n';
    return 0;
}

int CheckAnimationCatalog(const std::filesystem::path& game, const std::filesystem::path& mods_path) {
    ModIndexOptions options; options.dds_only = true; options.include_animations = true;
    const auto mods = BuildModIndex(mods_path, options);
    if (!mods) { std::wcerr << L"ANIM_CATALOG_FAILED: mod index\n"; return 1; }
    const auto built = BuildAnimationCandidates(game, *mods.snapshot);
    for (const auto& issue : built.issues) std::wcerr << L"ANIM_REJECTED: " << issue << L'\n';
    if (!built.snapshot || !built.error.empty()) {
        std::wcerr << L"ANIM_CATALOG_FAILED: " << built.error << L'\n'; return 1;
    }
    for (const auto& entry : built.snapshot->entries) {
        std::wcout << L"ANIM_CANDIDATE path=" << entry.virtual_path.key
                   << L" package_base=" << entry.identity.package_base
                   << L" table=" << entry.identity.member_table_offset
                   << L" member=" << entry.identity.member_ordinal
                   << L" bytes=" << entry.storage->Size()
                   << L" tracks=" << entry.layout.tracks
                   << L" changed_tracks=" << entry.changed_tracks << L'\n';
        for (const auto& range : entry.ranges)
            std::wcout << L"  range offset=" << range.offset << L" size=" << range.size
                       << L" original_sha256=" << Sha256HexWide(range.original_hash)
                       << L" replacement_sha256=" << Sha256HexWide(range.replacement_hash) << L'\n';
    }
    const auto expected = std::count_if(mods.snapshot->Assets().begin(), mods.snapshot->Assets().end(),
        [](const auto& asset) { return IsNativeAnimationPath(asset.virtual_path.key); });
    if (!built.issues.empty() || expected == 0 || built.snapshot->entries.size() != static_cast<std::size_t>(expected))
        return 1;
    std::wcout << L"ANIM_CATALOG_PASS count=" << built.snapshot->entries.size() << L" (offline only)\n";
    return 0;
}

// Parser parity over an extracted corpus: every clip must parse and validate
// as an unchanged keys_in_place edit of itself.
int ScanAnimationCorpus(const std::filesystem::path& directory, const std::filesystem::path& report) {
    std::vector<std::filesystem::path> paths;
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
        if (!item.is_regular_file()) continue;
        auto extension = item.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension == L".anm") paths.push_back(item.path());
    }
    std::sort(paths.begin(), paths.end());
    std::ofstream out;
    if (!report.empty()) {
        out.open(report, std::ios::binary);
        out << "path\tbytes\ttracks\tkeys\tmax_rotation_magnitude\tstatus\n";
    }
    std::uint64_t bytes = 0, tracks = 0, keys = 0, rejected = 0, above_unit = 0;
    float largest = 0;
    for (const auto& path : paths) {
        const auto loaded = LoadByteStorage(path, 64ull * 1024 * 1024);
        const auto relative = path.lexically_relative(directory).generic_u8string();
        AnimationValidationResult parsed;
        if (!loaded) parsed.error = L"read failed: " + loaded.detail;
        else {
            parsed = ParseAnimation(loaded.storage->Bytes());
            if (parsed) {
                const auto self = ValidateAnimationEdit(loaded.storage->Bytes(), loaded.storage->Bytes());
                if (!self || self.changed_tracks != 0) parsed.error = L"unchanged self-edit rejected: " + self.error;
            }
        }
        if (loaded) bytes += loaded.storage->Size();
        if (parsed) {
            tracks += parsed.layout.tracks;
            keys += parsed.layout.keys;
            largest = (std::max)(largest, parsed.layout.max_rotation_magnitude);
            if (parsed.layout.max_rotation_magnitude > 1.0f) ++above_unit;
        } else if (++rejected <= 20) {
            std::wcerr << L"ANIM_CORPUS_REJECTED path=" << path.lexically_relative(directory).generic_wstring()
                       << L" reason=" << parsed.error << L'\n';
        }
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(relative.data()), static_cast<std::streamsize>(relative.size()));
            out << '\t' << (loaded ? loaded.storage->Size() : 0) << '\t' << parsed.layout.tracks << '\t'
                << parsed.layout.keys << '\t' << parsed.layout.max_rotation_magnitude << '\t'
                << (parsed ? "decoded" : "rejected") << '\n';
        }
    }
    std::wcout << L"ANIM_CORPUS files=" << paths.size() << L" bytes=" << bytes
               << L" decoded=" << (paths.size() - rejected) << L" rejected=" << rejected
               << L" tracks=" << tracks << L" keys=" << keys
               << L" max_rotation_magnitude=" << largest << L" clips_above_unit=" << above_unit << L'\n';
    return rejected == 0 && !paths.empty() && (!out.is_open() || out.good()) ? 0 : 1;
}
