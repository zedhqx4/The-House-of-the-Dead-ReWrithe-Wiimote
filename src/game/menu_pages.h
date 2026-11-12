// The menu's settings pages (user 2026-10-20: the main menu's overhaul): our settings (LaunchConfig, launcher.ini)
// grouped into the categories Video, Audio, Controls, Gameplay, Mods and System, each a page of the PC-styled
// Frontend::screenExtras with a tab strip, and the Co-op page. Each row has its one-line help. The rows read and change
// the LaunchConfig they are given (the host's menu.cfg; --frontend-shot: the defaults) and call the host's hooks
// (save, the sub-pages, the actions that need the host); every value and launcher.ini key is the one the More Options
// page had (no behaviour change).
#pragma once

#include "assets/mods.h"
#include "game/frontend.h"
#include "game/launcher.h"
#include "game/sights.h"

#include <functional>
#include <string>
#include <vector>

namespace hotd::game {

enum MenuCategory { kCatVideo, kCatAudio, kCatControls, kCatGameplay, kCatMods, kCatSystem, kMenuCategories };
// menufb: the pages with a Reset to Defaults row past the categories: Gameplay's Advanced page, the Firing Range
enum : int { kResetAdvanced = 100, kResetRange = 101 };
// menufb: page's settings (a category, kResetAdvanced, kResetRange) back to their defaults (LaunchConfig's)
void menuReset(LaunchConfig& c, int page);
const char* menuCategoryName(int category);

struct MenuHooks {
    std::function<void()> save;                         // launcher.ini + the live settings (music on, numbering ...)
    std::function<void(int player)> padPage;            // a player's Controller page
    std::function<void()> bindPage;                     // keybind: the Keys & Buttons page (game/keybinds.h)
    std::function<void(int player)> bindPlayer;         // keybind: that page for another player
    std::function<PadBinds(int player)> padDefaults;    // keybind: the player's pad's buttons with its style (Pads)
    std::function<int(int player)> padType; // keybind: the player's pad's prompt style (pad_prompts.h: the buttons' names)
    std::function<void()> padsChanged;                  // keybind: the pads take the new buttons (Pads::configure)
    std::function<int()> promptStyleNow;                // padfix: the pad family the prompts show now (<= 0: none)
    std::function<void()> steamPage;                    // Add to Steam's pictures
    std::function<void()> crtPage;                      // the CRT filter's page (render/crt.h)
    std::function<void()> rangePage;                    // menufb: the Firing Range page (Video > Crosshairs)
    std::function<void(int page)> resetPage;            // menufb: a page's Reset to Defaults (ask, then menuReset)
    std::function<void()> languagePage;                 // menufb: System > Language's page (menuLanguageRows)
    std::function<void()> advancedPage;                 // advopt: the Advanced page (Gameplay's last row)
    std::function<void()> totdPage;                     // totdex: Gameplay > Typing of the Dead 0's page (null: no row)
    std::function<bool()> vrDualDefault;                // vr4: a VR run: vr.ini's dual_pistols (null: flat, no Dual Pistols row)
    std::function<bool()> vrLaserDefault;               // vrsight: a VR run: vr.ini's laser (null: flat, no Laser (VR) row)
    std::function<bool()> vrCrosshairDefault;           // vrsight: a VR run: vr.ini's crosshair (null: no Crosshair (VR) row)
    // vrcomfort: a VR run: vr.ini's value for the comfort rows (0 view, 1 cutscenes, 2 vignette, 3 the screen's distance
    // in cm, 4 its size: 0 small / 1 medium / 2 large, -1 none of them); null: flat, no rows
    std::function<int(int which)> vrComfortDefault;
    std::function<void()> filesCheck;                   // the text launcher (files check, data folders)
    std::function<void()> whatsNew;                     // setup2: the What's New page (the setup window)
    std::function<void()> shortcut;                     // create the desktop shortcut
    std::function<std::string()> shortcutValue;         // "Create" / "Created" / ...
    std::function<std::vector<Frontend::Extra>()> modsRows;          // the mods' rows (menuModRows with the host's mods)
    std::function<std::vector<CustomSight>()> customSights;          // the crosshairs folders' sets
    std::function<std::string()> crosshairDir;          // feat29: the folder players put their crosshair PNGs in (made)
    std::function<const assets::PcSprites*()> sightSprites;          // optui: the PC sprites (its sight; null: the arcade style drawn)
    std::function<void()> menuMusicChanged;             // the menu's song (the front end's cue)
    std::function<bool()> spanishVoices;                // spanish: a Spanish PC copy's voices are there (Audio > Voice)
    // rescrash: Video > Display Mode: the window's mode now (0 Windowed, 1 Borderless, 2 Fullscreen; util/resolution.h)
    // and into another one (applied at once, saved as the start's)
    std::function<int()> displayModeNow;
    std::function<void(int mode)> displayModeSet;
    std::function<void()> languageChanged;              // lang: System > Language changed (LaunchConfig::language)
    std::function<void()> menuStyleChanged;             // remakemenu: Menu Style changed (the front end's style)
    std::function<std::string()> liveBgAuto;            // menuperf: what Live Background's Auto does here ("on", "off: ...")
    std::function<std::string()> menuFpsAuto;           // optui: Menu Frame Rate's Auto here and now ("120 fps")
    std::function<std::string()> resolutionValue;       // Video > Resolution: the current mode's choice ("Auto", "1920 x 1080")
    std::function<void()> resolutionPage;               // its page: Auto and the sizes to pick from (util/resolution.h)
    // remakefix: Video > Display: the monitor shown on now (its label), every monitor's label, onto the next one / the
    // one with that label (applied at once, saved)
    std::function<std::string()> displayValue;
    std::function<std::vector<std::string>()> displayLabels;
    std::function<void()> displayNext;
    std::function<void(const std::string& label)> displayPick;
    std::function<void()> online;                       // the Online Co-op pages
    std::function<void(int players)> startCoop;         // Co-op: start the arcade with this many players
    // ipolish: Controls > Light Guns' Assign page; a seat waiting for a trigger pull (and the seconds it has left);
    // whether an assigned device is plugged in
    std::function<void()> gunPage;
    // Native Wii Remote setup: connection scan, per-player status and four-point IR calibration page.
    std::function<void()> wiimotePage;
    // fix28: a comparison picture "<kind>_<choice>" (models / blood: Frontend::Extra::preview): "mem:<key>" when it
    // can be drawn (Frontend::previewImage), else "" (its render asked for, menus3: in-process); "": "x" while
    // pictures can be made, "" not
    std::function<std::string(const std::string& name)> previewPic;
    std::function<void(int seat)> gunAssign;
    std::function<int()> gunWaiting;
    std::function<int()> gunWaitLeft;
    std::function<bool(const std::string& key)> gunConnected;
    std::function<std::string()> gunNote;   // linuxmice: why no gun / mouse can be read (Linux: the input group), "": fine
};

// questui (user 2026-11-10: "a lot of the settings don't apply to Meta Quest"; hidden, not greyed): the standalone
// Quest's menus: the rows that do nothing there left out (the window, the monitor, the renderer, the CRT filter, the
// mouse / keyboard / pads / light guns, co-op, the PC's shortcuts and updates; docs/quest.md "Settings on the Quest"),
// the VR rows on Controls. On on Android; on the PC only with --quest-sim / HOTD_QUEST_MENUS=1 (tests). The PC's own
// menus (PC VR too) are unchanged
extern bool menuQuest;
// the rows of a category's page
std::vector<Frontend::Extra> menuCategoryRows(int category, LaunchConfig& c, const MenuHooks& h);
// fxux: a settings page's rows opened from the pause (Frontend::rowsFilter): "from the next game" notes, the rows a
// game cannot change grey (online: every game rule too)
void menuInGameRows(std::vector<Frontend::Extra>& rows, const std::string& title, bool online);
// menus3 (preview_art.h): a comparison picture "<kind>_<choice>" (models_0 / 1, blood_0 .. 5) as its in-memory key
// (the models, the blood colour, player 1's crosshair: a changed one is another picture); "": no such picture
std::string previewKey(const std::string& name, const LaunchConfig& c, bool pcOk);
// the frame the game rendered for those models (0 arcade, 1 PC), kept in the pictures' folder: kind
// kPreviewBlood base_<arcade|pc>.png (the attract), kPreviewModels (prevbridge) bridge_<arcade|pc>.png (Chapter 1)
std::string previewBaseFile(const std::string& dir, int models, int kind = 0, int variant = 0);
// fxux: kPreviewMarks marks_<arcade|pc>_<0 Arcade Dots / Solid, 1 Smooth>.png and kPreviewTransp
// transp_arcade_<Transparency mode 0..2>.png; the base file of an in-memory key ("" none)
std::string previewKeyFile(const std::string& dir, const std::string& key);
// the picture of a key: the base with the crosshair (and the blood burst); false: no base yet
bool previewRender(const std::string& key, const std::string& dir, const LaunchConfig& c, const MenuHooks& h,
                   std::vector<uint8_t>& rgba, int& w, int& hgt);
// mods (docs/modding.md): the mods in the Mods page's order (the On ones by priority, launcher.ini mods=, then the
// others; a mod in neither list: its mod.ini default) and whether each is on
std::vector<std::pair<const ModPack*, bool>> modOrder(const LaunchConfig& c, const std::vector<ModPack>& mods);
// the Mods page's rows: per mod its name (On / Off), Settings (its page: openSettings(id)), Move Up. changed: after
// LaunchConfig's mods= / modsoff= / modset. changed (save, apply). mods and c are kept by reference
std::vector<Frontend::Extra> menuModRows(LaunchConfig& c, const std::vector<ModPack>& mods, std::function<void()> changed,
                                         std::function<void(const std::string& id)> openSettings);
// leftovers: the Mods page's rows clicked on made-up mods (texture packs off by default, the Ragdoll), each click
// checked against the mods it should switch (hotd --mods-test); prints the checks, returns the failures
int modsPageTest();
// a mod's Settings page: one row per [settings] line (its choices), then Defaults
std::vector<Frontend::Extra> menuModSettingRows(LaunchConfig& c, const ModPack& mod, std::function<void()> changed);
// advopt: the Advanced page's rows (Game Speed, Music Numbering, Teammate Flashes, 4P Boss Hit Points, Guards; the
// same values and launcher.ini keys they had on Video, Audio and Gameplay)
std::vector<Frontend::Extra> menuAdvancedRows(LaunchConfig& c, const MenuHooks& h);
// menufb: the Firing Range page's rows for player seat (its Player row changes seat): the crosshair, colour, size,
// thickness, Copy to Every Player (the page shows the range under them: Frontend::extrasRange)
std::vector<Frontend::Extra> menuRangeRows(LaunchConfig& c, const MenuHooks& h, int& seat);
// menufb: the Language page's rows: each language by its own name, Current / Sample, a line of it as the help
std::vector<Frontend::Extra> menuLanguageRows(LaunchConfig& c, const MenuHooks& h);
// relmenu: a Language row's name ("한국어 (Korean)"; a left-to-right mark first for a right-to-left name in a
// left-to-right menu), and whether a text is one of them (not a key to translate: --export-lang / --lang-check)
std::string menuLanguageName(const std::string& name, const std::string& english);
bool isMenuLanguageName(const std::string& text);
// the Co-op page's rows (Local 2 Players, 4 Players, Online Co-op, Players)
std::vector<Frontend::Extra> menuCoopRows(LaunchConfig& c, const MenuHooks& h);
// keybind: the Keys & Buttons page's rows for player p (page 1 the keyboard, page 2 the gamepad); a row clicked starts
// cap's "press a key / button" (the host hands it the events: the menu loop)
std::vector<Frontend::Extra> menuBindRows(int p, LaunchConfig& c, BindCapture& cap, const MenuHooks& h);
// ipolish: Controls > Light Guns' Assign page: per seat P1-P4 its device (a click: the seat waits for a trigger pull
// from any gun / mouse, h.gunAssign) and Clear; then Clear All (LaunchConfig::gunDev / gunName, saved by h.save)
std::vector<Frontend::Extra> menuGunAssignRows(LaunchConfig& c, const MenuHooks& h);
// the help line of a Controller page's field (PadField::key)
const char* padFieldHelp(const char* key);
// uxpolish: a light gun's calibration row (Controls, a Controller page): "Reset" when calibrated (cal = the player's
// LaunchConfig::gunCal), else grey "Not Calibrated" with how to (F9 in the game); tagged Experimental. Appended to r
std::vector<Frontend::Extra>& lightGunRow(std::vector<Frontend::Extra>& r, const std::string& label, float* cal,
                                          std::function<void()> save);

} // namespace hotd::game

namespace hotd::game {
// High Scores' tables: 0-3 the presets of Sonic Mode (0 the arcade's own), 4-7 the same for the 4 players mode
// (table t: preset t % 4, 4P t >= 4)
std::vector<std::string> menuRankingTables();
} // namespace hotd::game
