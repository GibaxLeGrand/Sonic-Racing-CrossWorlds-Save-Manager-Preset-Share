#include <algorithm>
#include "Core/Files.h"
#include "Core/System.h"
#include "Data/GadgetCatalog.h"
#include "Localization/Localization.h"
#include "Windows/Dialogs/MessageDialog.h"
#include "Windows/Main/MainWindow.h"
#include "Windows/Main/Tabs/SaveEditor/SaveEditorTab.h"

// Sharing gadget presets (Services/Share/PresetShare): a preset's code to the clipboard and a code from it
// onto a preset; the shown player's presets to a .srcwshare file and back. Imports check the game version
// the share was made with, put on only what the save has unlocked, and say what was left out.
namespace SonicRacingSaveManager::Main {

namespace SF = Services::SaveFile;
namespace SH = Services::Share;

namespace {

std::wstring PresetLabel(int index) { return Loc::Fmt("saveeditor_gadget_preset", std::wstring(1, static_cast<wchar_t>(L'A' + index))); }

std::vector<System::FileFilter> ShareFilters() {
    return { { Loc::Get("share_filter"), L"*.srcwshare" }, { Loc::Get("filter_all"), L"*.*" } };
}

std::wstring FileName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

} // namespace

void SaveEditorTab::CopyPresetCode(const SF::GadgetPreset& preset) {
    if (!save) return;
    const std::wstring code = SH::EncodeCode(preset, SH::GameVersion::FromRom(save->RomVersion()));
    if (System::SetClipboardText(code)) Status(Loc::Fmt("share_code_copied", PresetLabel(preset.index)));
    else Status(Loc::Get("share_clipboard_error"), true);
}

void SaveEditorTab::PastePresetCode(int preset) {
    if (!save || busy) return;
    std::wstring error;
    const auto decoded = SH::DecodeCode(System::GetClipboardText(), error);
    if (!decoded) {
        Status(error, true);
        return;
    }
    PutShared({ { preset, decoded->preset } }, decoded->version, decoded->missing, 0);
}

void SaveEditorTab::ExportPresets() {
    if (!save || busy) return;
    // Every preset, the empty ones too: putting the file back gives the same presets
    std::vector<SH::SharedPreset> out;
    for (const auto& p : GadgetPresets()) {
        SH::SharedPreset s;
        s.player = gadgetPlayer;
        s.preset = p.index;
        for (const auto& g : p.gadgets) s.gadgets.push_back({ g.id, g.slot, g.size });
        if (!p.special.empty()) s.special = SH::SharedGadget{ p.special, SF::kGadgetSpecialSlot, Data::GadgetCatalog::Slots(p.special) };
        out.push_back(std::move(s));
    }
    if (out.empty()) return;
    const std::wstring path = System::PickSaveFile(main.Hwnd(), Loc::Get("share_export_title"), ShareFilters(),
                                                   Loc::Fmt("share_export_file", std::to_wstring(gadgetPlayer + 1)), L"srcwshare");
    if (path.empty()) return;
    if (Files::WriteBytes(path, SH::ExportFile(out, SH::GameVersion::FromRom(save->RomVersion()))))
        Status(Loc::Fmt("share_exported", std::to_wstring(out.size()), FileName(path)));
    else
        Status(Loc::Get("share_write_error"), true);
}

void SaveEditorTab::ImportPresets() {
    if (!save || busy) return;
    const std::wstring path = System::PickFile(main.Hwnd(), Loc::Get("share_import_title"), ShareFilters());
    if (path.empty()) return;
    const auto bytes = Files::ReadBytes(path);
    if (!bytes) {
        Status(Loc::Get("share_read_error"), true);
        return;
    }
    std::wstring error;
    const auto file = SH::ImportFile(*bytes, error);
    if (!file) {
        Status(error, true);
        return;
    }
    // Each preset goes back on its letter (of the shown player); a file holding one preset without a letter
    // goes on the edited one
    const int presets = static_cast<int>(GadgetPresets().size());
    std::vector<std::pair<int, SH::SharedPreset>> targets;
    for (const auto& p : file->presets) {
        const int to = p.preset >= 0 ? p.preset : file->presets.size() == 1 ? std::max(gadgetPreset, 0) : -1;
        if (to >= 0 && to < presets) targets.emplace_back(to, p);
    }
    if (targets.empty()) {
        Status(Loc::Get("share_nothing"), true);
        return;
    }
    PutShared(std::move(targets), file->version, 0, file->skipped);
}

void SaveEditorTab::PutShared(std::vector<std::pair<int, SH::SharedPreset>> targets, const SH::GameVersion& from, int missing,
                              int skipped) {
    if (!save || busy || targets.empty()) return;
    const SH::GameVersion ours = SH::GameVersion::FromRom(save->RomVersion());
    const SH::Age age = SH::AgeOf(from, ours);
    if (age == SH::Age::Newer) {
        Status(Loc::Fmt("share_newer", from.Text(), ours.Text()), true);
        return;
    }

    // What a game update changed in the shared gadgets (their sizes, or gone): said before putting them on
    std::wstring changes;
    if (age != SH::Age::Same)
        for (const auto& [to, p] : targets)
            for (const SH::Change& c : SH::Changes(p))
                changes += L"\n- " + (c.now == 0 ? Loc::Fmt("share_change_gone", GadgetName(c.id))
                                                 : Loc::Fmt("share_change_size", GadgetName(c.id), std::to_wstring(c.then), std::to_wstring(c.now)));

    // The presets it replaces
    const auto& now = GadgetPresets();
    std::wstring replaced;
    for (const auto& [to, p] : targets)
        if (to < static_cast<int>(now.size()) && (!now[static_cast<size_t>(to)].gadgets.empty() || !now[static_cast<size_t>(to)].special.empty()))
            replaced += (replaced.empty() ? L"" : L", ") + PresetLabel(to);

    const int player = gadgetPlayer;
    auto put = [this, targets = std::move(targets), player, missing, skipped] {
        if (!save || busy) return;
        SH::ApplyResult all;
        const auto add = [](std::vector<std::wstring>& into, const std::vector<std::wstring>& from) { into.insert(into.end(), from.begin(), from.end()); };
        Edit([&] {
            for (const auto& [to, p] : targets) {
                const SH::ApplyResult r = SH::Apply(*save, player, to, p);
                all.placed += r.placed;
                add(all.locked, r.locked);
                add(all.unknown, r.unknown);
                add(all.noRoom, r.noRoom);
            }
        });
        gadgetPreset = targets.size() == 1 ? targets.front().first : gadgetPreset;
        const auto names = [this](const std::vector<std::wstring>& ids) {
            std::wstring s;
            for (const auto& id : ids) s += (s.empty() ? L"" : L", ") + GadgetName(id);
            return s;
        };
        std::wstring text = targets.size() == 1 ? Loc::Fmt("share_put_one", PresetLabel(targets.front().first), std::to_wstring(all.placed))
                                                : Loc::Fmt("share_put_many", std::to_wstring(targets.size()), std::to_wstring(all.placed));
        if (!all.locked.empty()) text += L" " + Loc::Fmt("share_left_locked", names(all.locked));
        if (!all.noRoom.empty()) text += L" " + Loc::Fmt("share_left_room", names(all.noRoom));
        if (const size_t unknown = all.unknown.size() + static_cast<size_t>(missing); unknown > 0)
            text += L" " + Loc::Fmt("share_left_unknown", std::to_wstring(unknown));
        if (skipped > 0) text += L" " + Loc::Fmt("share_skipped", std::to_wstring(skipped));
        Status(text, !all.locked.empty() || !all.noRoom.empty() || all.unknown.size() + static_cast<size_t>(missing) > 0);
    };

    if (replaced.empty() && changes.empty()) {
        put();
        return;
    }
    const auto said = [](const SH::GameVersion& v) { return v.Known() ? v.Text() : Loc::Get("share_version_unknown"); };
    std::wstring text;
    if (!replaced.empty()) text = Loc::Fmt("share_confirm_replace", replaced);
    if (!changes.empty()) text += (text.empty() ? L"" : L"\n\n") + Loc::Fmt("share_changed", said(from), said(ours)) + changes;
    text += L"\n\n" + Loc::Get("share_confirm");
    Dialogs::Message(main.Hwnd(), text, Loc::Get("share_title"), Dialogs::MessageButtons::YesNo,
                     changes.empty() ? Dialogs::MessageIcon::Question : Dialogs::MessageIcon::Warning, [put](Dialogs::MessageResult r) {
                         if (r == Dialogs::MessageResult::Yes) put();
                     });
}

} // namespace SonicRacingSaveManager::Main
