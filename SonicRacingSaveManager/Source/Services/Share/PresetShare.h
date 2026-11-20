#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Services/SaveFile/AppSave.h"

// Sharing gadget presets: a short text code for one preset (to paste in a chat) and a JSON file for
// several (".srcwshare"). The format is shared with GadgetTool (https://github.com/GibaxLeGrand/GadgetTool),
// so a preset made in either app imports in both. Both record the game version and each gadget's size,
// so an import after a game update can say what changed instead of silently breaking the plate.
//
// Code: "SRCW1-" + base64url (no padding) of these bytes:
//   0     format version: 1
//   1     kind: 1 gadget preset (2 machine build: not written yet)
//   2..4  game version: major, minor, patch (0.0.0 = unknown)
//   5     the plate's gadget count n (the special slot's not counted)
//   6     flags: bit 0, a special-slot gadget follows
//   then, for each gadget in slot order and then the special one: FNV-1a 32 of its id in lowercase
//   (4 bytes, little-endian) and one byte: its SlotIndex (bits 0..2) | the slots it took when shared
//   (bits 3..4, 0 = unknown)
//   last 2: checksum, FNV-1a 32 of everything before folded to 16 bits (h ^ h >> 16), little-endian
// A typo or a cut code fails the checksum instead of decoding into another preset.
namespace SonicRacingSaveManager::Services::Share {

// The game version a share was made with ("2.0.0"); 0.0.0 when unknown
struct GameVersion {
    int major = 0, minor = 0, patch = 0;
    bool Known() const { return major || minor || patch; }
    std::wstring Text() const;
    // From the save's _Header._RomVersion: 20000 is 2.0.0 (major * 10000 + minor * 100 + patch)
    static GameVersion FromRom(int64_t rom);
    static GameVersion Parse(std::wstring_view text);
};

// A share's version against the save's: the same (nothing to check), older (its gadgets are checked
// against this app's list), newer (refused: it may hold what this version doesn't have), unknown (checked)
enum class Age { Same, Older, Newer, Unknown };
Age AgeOf(const GameVersion& shared, const GameVersion& ours);

struct SharedGadget {
    std::wstring id;   // EGadgetId value name
    int slot = 0;      // its first slot (SlotIndex; 6 is the special slot)
    int size = 0;      // the slots it took when shared (0: unknown)
};

struct SharedPreset {
    std::wstring name;              // optional
    int player = -1, preset = -1;   // where it came from (0-based; -1: unknown)
    std::vector<SharedGadget> gadgets;   // on the plate, by slot
    std::optional<SharedGadget> special;
};

// One preset as a code
std::wstring EncodeCode(const SaveFile::GadgetPreset& preset, const GameVersion& version);

struct DecodedCode {
    SharedPreset preset;
    GameVersion version;
    int missing = 0;   // gadgets this app doesn't know (left out)
};
// nullopt, with `error` set, when the text is not a code this app can read
std::optional<DecodedCode> DecodeCode(std::wstring_view text, std::wstring& error);

// Presets as a file (UTF-8 JSON)
std::string ExportFile(const std::vector<SharedPreset>& presets, const GameVersion& version);

struct SharedFile {
    GameVersion version;
    std::vector<SharedPreset> presets;
    int skipped = 0;   // items of other kinds (machine builds) this version doesn't read
};
std::optional<SharedFile> ImportFile(std::string_view json, std::wstring& error);

// A gadget whose size changed since it was shared, or that this app doesn't know (now 0)
struct Change {
    std::wstring id;
    int then = 0, now = 0;
};
std::vector<Change> Changes(const SharedPreset& preset);

// Puts a shared preset on a player's preset: empties it, then puts on each gadget the save has unlocked,
// at its slot when free, else where it first fits, then the special slot. Nothing is unlocked: what could
// not be put on is listed (ids).
struct ApplyResult {
    int placed = 0;
    std::vector<std::wstring> locked;    // not unlocked in this save
    std::vector<std::wstring> unknown;   // not in this app's list of gadgets
    std::vector<std::wstring> noRoom;    // no free place left, or the special slot is still closed
};
ApplyResult Apply(SaveFile::AppSave& save, int player, int preset, const SharedPreset& shared);

} // namespace SonicRacingSaveManager::Services::Share
