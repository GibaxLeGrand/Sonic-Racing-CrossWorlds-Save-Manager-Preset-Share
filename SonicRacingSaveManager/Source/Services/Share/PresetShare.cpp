#include "Services/Share/PresetShare.h"
#include <algorithm>
#include <map>
#include "Core/Json.h"
#include "Core/Text.h"
#include "Data/GadgetCatalog.h"
#include "Localization/Localization.h"

namespace SonicRacingSaveManager::Services::Share {

namespace {

namespace GC = Data::GadgetCatalog;
namespace SF = SaveFile;

constexpr std::wstring_view kPrefix = L"SRCW1-";
constexpr unsigned char kFormat = 1, kKindGadgetPreset = 1, kKindMachineBuild = 2, kHasSpecial = 1;
constexpr size_t kHeader = 7, kPerGadget = 5, kChecksum = 2;
constexpr const char* kFileFormat = "srcw.share";

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return out;
}

uint32_t Fnv1a(std::string_view bytes) {
    uint32_t h = 2166136261u;
    for (const unsigned char b : bytes) {
        h ^= b;
        h *= 16777619u;
    }
    return h;
}

// Ids are compared ignoring case (the game spells some two ways), so their hash is of the lowercase id
uint32_t IdHash(std::wstring_view id) { return Fnv1a(Text::Narrow(Lower(id))); }

uint16_t Checksum(std::string_view bytes) {
    const uint32_t h = Fnv1a(bytes);
    return static_cast<uint16_t>(h ^ (h >> 16));
}

void PutU32(std::string& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}
uint32_t GetU32(std::string_view in, size_t at) {
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[at + k])) << (8 * k);
    return v;
}

constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::wstring ToBase64Url(std::string_view bytes) {
    std::wstring out;
    size_t k = 0;
    for (; k + 3 <= bytes.size(); k += 3) {
        const uint32_t v = (static_cast<unsigned char>(bytes[k]) << 16) | (static_cast<unsigned char>(bytes[k + 1]) << 8) |
                           static_cast<unsigned char>(bytes[k + 2]);
        for (int s = 18; s >= 0; s -= 6) out.push_back(static_cast<wchar_t>(kBase64Url[(v >> s) & 63]));
    }
    if (const size_t rest = bytes.size() - k; rest > 0) {
        uint32_t v = static_cast<unsigned char>(bytes[k]) << 16;
        if (rest == 2) v |= static_cast<unsigned char>(bytes[k + 1]) << 8;
        out.push_back(static_cast<wchar_t>(kBase64Url[(v >> 18) & 63]));
        out.push_back(static_cast<wchar_t>(kBase64Url[(v >> 12) & 63]));
        if (rest == 2) out.push_back(static_cast<wchar_t>(kBase64Url[(v >> 6) & 63]));
    }
    return out;
}

// nullopt on a character a code can't hold, or a length no bytes give
std::optional<std::string> FromBase64Url(std::wstring_view text) {
    if (text.size() % 4 == 1) return std::nullopt;
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (const wchar_t c : text) {
        int d;
        if (c >= L'A' && c <= L'Z') d = c - L'A';
        else if (c >= L'a' && c <= L'z') d = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') d = c - L'0' + 52;
        else if (c == L'-') d = 62;
        else if (c == L'_') d = 63;
        else return std::nullopt;
        v = (v << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((v >> bits) & 0xFF));
        }
    }
    return out;
}

// A gadget id as this app's list spells it; empty when the list doesn't have it
std::wstring KnownId(std::wstring_view id) {
    for (const auto& g : GC::All())
        if (GC::SameId(id, g.id)) return g.id;
    for (const auto& g : GC::Specials())
        if (GC::SameId(id, g.id)) return g.id;
    return {};
}

const std::map<uint32_t, std::wstring>& ByHash() {
    static const std::map<uint32_t, std::wstring> map = [] {
        std::map<uint32_t, std::wstring> m;
        for (const auto& g : GC::All()) m.emplace(IdHash(g.id), g.id);
        for (const auto& g : GC::Specials()) m.emplace(IdHash(g.id), g.id);
        return m;
    }();
    return map;
}

// The slots a gadget takes now, as two bits (0 when it doesn't fit them or isn't known)
unsigned SizeBits(std::wstring_view id) {
    if (KnownId(id).empty()) return 0;
    const int size = GC::Slots(id);
    return size >= 1 && size <= 3 ? static_cast<unsigned>(size) : 0u;
}

void PutGadget(std::string& out, std::wstring_view id, int slot) {
    PutU32(out, IdHash(id));
    out.push_back(static_cast<char>((static_cast<unsigned>(slot) & 7u) | (SizeBits(id) << 3)));
}

} // namespace

// ── Game versions ───────────────────────────────────────────────────────────────────────────────

std::wstring GameVersion::Text() const {
    return std::to_wstring(major) + L"." + std::to_wstring(minor) + L"." + std::to_wstring(patch);
}

GameVersion GameVersion::FromRom(int64_t rom) {
    if (rom <= 0 || rom > 2559999) return {};
    return { static_cast<int>(rom / 10000), static_cast<int>(rom / 100 % 100), static_cast<int>(rom % 100) };
}

GameVersion GameVersion::Parse(std::wstring_view text) {
    GameVersion v;
    int* parts[3] = { &v.major, &v.minor, &v.patch };
    size_t k = 0;
    int n = 0;
    bool digit = false;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            n = n * 10 + (c - L'0');
            digit = true;
            if (n > 255) return {};
        } else if (c == L'.' && digit && k < 2) {
            *parts[k++] = n;
            n = 0;
            digit = false;
        } else {
            return {};
        }
    }
    if (!digit) return {};
    *parts[k] = n;
    return v;
}

Age AgeOf(const GameVersion& shared, const GameVersion& ours) {
    if (!shared.Known() || !ours.Known()) return Age::Unknown;
    const auto key = [](const GameVersion& v) { return v.major * 65536 + v.minor * 256 + v.patch; };
    return key(shared) < key(ours) ? Age::Older : key(shared) > key(ours) ? Age::Newer : Age::Same;
}

// ── Codes ───────────────────────────────────────────────────────────────────────────────────────

std::wstring EncodeCode(const SF::GadgetPreset& preset, const GameVersion& version) {
    std::vector<const SF::PlacedGadget*> plate;
    for (const auto& g : preset.gadgets)
        if (g.slot >= 0 && g.slot < SF::kGadgetPlate && plate.size() < static_cast<size_t>(SF::kGadgetPlate)) plate.push_back(&g);
    std::string bytes;
    bytes.push_back(static_cast<char>(kFormat));
    bytes.push_back(static_cast<char>(kKindGadgetPreset));
    for (const int part : { version.major, version.minor, version.patch }) bytes.push_back(static_cast<char>(std::clamp(part, 0, 255)));
    bytes.push_back(static_cast<char>(plate.size()));
    bytes.push_back(static_cast<char>(preset.special.empty() ? 0 : kHasSpecial));
    for (const auto* g : plate) PutGadget(bytes, g->id, g->slot);
    if (!preset.special.empty()) PutGadget(bytes, preset.special, SF::kGadgetSpecialSlot);
    const uint16_t sum = Checksum(bytes);
    bytes.push_back(static_cast<char>(sum & 0xFF));
    bytes.push_back(static_cast<char>(sum >> 8));
    return std::wstring(kPrefix) + ToBase64Url(bytes);
}

std::optional<DecodedCode> DecodeCode(std::wstring_view text, std::wstring& error) {
    const std::wstring trimmed = Text::Trim(std::wstring(text));
    if (trimmed.size() < kPrefix.size() || Lower(trimmed.substr(0, kPrefix.size())) != Lower(kPrefix)) {
        error = Loc::Get("share_error_not_code");
        return std::nullopt;
    }
    const auto decoded = FromBase64Url(std::wstring_view(trimmed).substr(kPrefix.size()));
    if (!decoded || decoded->size() < kHeader + kChecksum) {
        error = Loc::Get("share_error_damaged");
        return std::nullopt;
    }
    const std::string& b = *decoded;
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(b[k]); };
    if (byte(0) != kFormat) {
        error = Loc::Fmt("share_error_format", std::to_wstring(byte(0)));
        return std::nullopt;
    }
    if (byte(1) == kKindMachineBuild) {
        error = Loc::Get("share_error_machine");
        return std::nullopt;
    }
    const size_t count = byte(5);
    const bool hasSpecial = (byte(6) & kHasSpecial) != 0;
    const size_t total = count + (hasSpecial ? 1 : 0);
    if (byte(1) != kKindGadgetPreset || count > static_cast<size_t>(SF::kGadgetPlate) || (byte(6) & ~kHasSpecial) != 0 ||
        b.size() != kHeader + kPerGadget * total + kChecksum) {
        error = Loc::Get("share_error_damaged");
        return std::nullopt;
    }
    const uint16_t sum = static_cast<uint16_t>(byte(b.size() - 2) | (byte(b.size() - 1) << 8));
    if (sum != Checksum(std::string_view(b).substr(0, b.size() - kChecksum))) {
        error = Loc::Get("share_error_checksum");
        return std::nullopt;
    }

    DecodedCode out;
    out.version = { byte(2), byte(3), byte(4) };
    for (size_t k = 0; k < total; ++k) {
        const size_t at = kHeader + kPerGadget * k;
        const auto found = ByHash().find(GetU32(b, at));
        const unsigned info = byte(at + 4);
        if (found == ByHash().end()) {
            ++out.missing;
            continue;
        }
        SharedGadget g{ found->second, static_cast<int>(info & 7u), static_cast<int>((info >> 3) & 3u) };
        if (k < count) out.preset.gadgets.push_back(std::move(g));
        else out.preset.special = std::move(g);
    }
    std::sort(out.preset.gadgets.begin(), out.preset.gadgets.end(), [](const auto& x, const auto& y) { return x.slot < y.slot; });
    return out;
}

// ── Files ───────────────────────────────────────────────────────────────────────────────────────

std::string ExportFile(const std::vector<SharedPreset>& presets, const GameVersion& version) {
    Json::Value root = Json::Value::MakeObject();
    root.Set("format", kFileFormat);
    root.Set("version", 1);
    if (version.Known()) root.Set("gameVersion", version.Text());

    // Every gadget's size as this app sees it now: an import after a game update compares them
    Json::Value sizes = Json::Value::MakeObject();
    Json::Value items = Json::Value::MakeArray();
    const auto record = [&](const std::wstring& id) {
        if (const std::wstring known = KnownId(id); !known.empty()) {
            Json::Value g = Json::Value::MakeObject();
            g.Set("slotCost", GC::Slots(known));
            sizes.Set(Text::Narrow(known), std::move(g));
        }
    };
    for (const auto& p : presets) {
        Json::Value item = Json::Value::MakeObject();
        item.Set("kind", "gadgetPreset");
        if (!p.name.empty()) item.Set("name", p.name);
        if (p.player >= 0) item.Set("player", p.player + 1);
        if (p.preset >= 0 && p.preset < 26) item.Set("preset", std::wstring(1, static_cast<wchar_t>(L'A' + p.preset)));
        Json::Value gadgets = Json::Value::MakeArray();
        for (const auto& g : p.gadgets) {
            Json::Value e = Json::Value::MakeObject();
            e.Set("id", g.id);
            e.Set("slot", g.slot);
            gadgets.Push(std::move(e));
            record(g.id);
        }
        item.Set("gadgets", std::move(gadgets));
        if (p.special) {
            item.Set("special", p.special->id);
            record(p.special->id);
        }
        items.Push(std::move(item));
    }
    root.Set("gadgets", std::move(sizes));
    root.Set("items", std::move(items));
    return root.Serialize(true);
}

std::optional<SharedFile> ImportFile(std::string_view json, std::wstring& error) {
    bool ok = false;
    const Json::Value root = Json::Parse(json, &ok);
    if (!ok || !root.IsObject() || root["format"].Str() != kFileFormat || !root["items"].IsArray()) {
        error = Loc::Get("share_error_not_file");
        return std::nullopt;
    }
    if (const long long v = root["version"].Int(); v > 1) {
        error = Loc::Fmt("share_error_file_version", std::to_wstring(v));
        return std::nullopt;
    }

    // The sizes the file recorded, by lowercase id
    std::map<std::wstring, int> sizes;
    for (const auto& [id, g] : root["gadgets"].Items()) sizes[Lower(Text::Widen(id))] = static_cast<int>(g["slotCost"].Int());
    const auto sizeOf = [&](const std::wstring& id) {
        const auto it = sizes.find(Lower(id));
        return it == sizes.end() ? 0 : it->second;
    };

    SharedFile out;
    out.version = GameVersion::Parse(root["gameVersion"].WStr());
    for (const Json::Value& item : root["items"].Elements()) {
        if (item["kind"].Str() != "gadgetPreset") {
            ++out.skipped;
            continue;
        }
        SharedPreset p;
        p.name = item["name"].WStr();
        if (const long long player = item["player"].Int(); player >= 1 && player <= SF::kGadgetPlayers) p.player = static_cast<int>(player - 1);
        if (const std::wstring letter = item["preset"].WStr(); letter.size() == 1 && letter[0] >= L'A' && letter[0] < L'A' + SF::kGadgetPresets)
            p.preset = letter[0] - L'A';
        for (const Json::Value& g : item["gadgets"].Elements()) {
            const std::wstring id = g["id"].WStr();
            const long long slot = g["slot"].Int(-1);
            if (id.empty() || slot < 0 || slot >= SF::kGadgetPlate) continue;
            p.gadgets.push_back({ id, static_cast<int>(slot), sizeOf(id) });
        }
        std::sort(p.gadgets.begin(), p.gadgets.end(), [](const auto& x, const auto& y) { return x.slot < y.slot; });
        if (const std::wstring special = item["special"].WStr(); !special.empty())
            p.special = SharedGadget{ special, SF::kGadgetSpecialSlot, sizeOf(special) };
        out.presets.push_back(std::move(p));
    }
    return out;
}

// ── Checking and putting on ─────────────────────────────────────────────────────────────────────

std::vector<Change> Changes(const SharedPreset& preset) {
    std::vector<Change> out;
    const auto check = [&](const SharedGadget& g) {
        if (g.size <= 0) return;
        const std::wstring known = KnownId(g.id);
        const int now = known.empty() ? 0 : GC::Slots(known);
        if (now != g.size) out.push_back({ known.empty() ? g.id : known, g.size, now });
    };
    for (const auto& g : preset.gadgets) check(g);
    if (preset.special) check(*preset.special);
    return out;
}

ApplyResult Apply(SF::AppSave& save, int player, int preset, const SharedPreset& shared) {
    ApplyResult r;
    // The save's own spelling of every gadget it has, and whether it is unlocked
    std::map<std::wstring, std::pair<std::wstring, bool>> inSave;
    for (const auto& row : save.Rows(SF::ListId::Gadgets)) inSave[Lower(row.id)] = { row.id, row.state == SF::ItemState::Unlocked };
    // 0 it can be put on (`id` the save's spelling), 1 not in this app's list, 2 not unlocked in this save
    const auto usable = [&](const std::wstring& wanted, std::wstring& id) {
        if (KnownId(wanted).empty()) return 1;
        const auto it = inSave.find(Lower(wanted));
        if (it == inSave.end() || !it->second.second) return 2;
        id = it->second.first;
        return 0;
    };

    save.ClearGadgetPreset(player, preset);
    for (const auto& g : shared.gadgets) {
        std::wstring id;
        if (const int why = usable(g.id, id); why != 0) {
            (why == 1 ? r.unknown : r.locked).push_back(g.id);
            continue;
        }
        // At its slot; when that's taken or still closed, where it first fits
        bool done = save.PlaceGadget(player, preset, id, g.slot);
        for (int s = 0; !done && s + GC::Slots(id) <= save.GadgetSlotCount(); ++s) done = save.PlaceGadget(player, preset, id, s);
        if (done) ++r.placed;
        else r.noRoom.push_back(id);
    }
    if (shared.special) {
        std::wstring id;
        if (const int why = usable(shared.special->id, id); why != 0) (why == 1 ? r.unknown : r.locked).push_back(shared.special->id);
        else if (save.SpecialSlotOpen() && save.SetSpecialGadget(player, preset, id)) ++r.placed;
        else r.noRoom.push_back(id);
    }
    return r;
}

} // namespace SonicRacingSaveManager::Services::Share
