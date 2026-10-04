#include "game_config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace rps {

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

static std::vector<std::string> split(const std::string &value, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(value);
  std::string tok;
  while (std::getline(ss, tok, delim))
    out.push_back(trim(tok));
  return out;
}

// Find an EBOOT.BIN inside a jailbreak game folder (common RPCS3 layouts).
static std::string find_eboot(const fs::path &dir) {
  static const char *rel[] = {
      "PS3_GAME/USRDIR/EBOOT.BIN",
      "USRDIR/EBOOT.BIN",
      "EBOOT.BIN",
  };
  for (const char *r : rel) {
    std::error_code ec;
    fs::path p = dir / r;
    if (fs::is_regular_file(p, ec))
      return p.string();
  }
  return "";
}

static bool is_pcsx2_image(const fs::path &p) {
  std::string ext = lower(p.extension().string());
  return ext == ".iso" || ext == ".bin" || ext == ".elf" || ext == ".cue" ||
         ext == ".img";
}

static void add_game(std::vector<GameEntry> &out, const fs::path &boot,
                     const std::string &name, EmulatorType emu) {
  GameEntry e;
  e.name = name;
  e.emulator = emu;
  e.boot_path = boot.string();
  out.push_back(std::move(e));
}

// True if `bin` is a track referenced by the cue sheet named `cueStem`
// (stem-equal, or stem-prefix like "game (Track 1)"). PCSX2 boots the .bin,
// not the .cue.
static bool bin_belongs_to_cue(const fs::path &bin, const std::string &cueStem) {
  std::string stem = lower(bin.stem().string());
  std::string cue = lower(cueStem);
  if (stem == cue)
    return true;
  if (stem.size() > cue.size() && stem.compare(0, cue.size(), cue) == 0) {
    char nxt = stem[cue.size()];
    return !(std::isalnum((unsigned char)nxt));
  }
  return false;
}

// Collect PCSX2 image files directly inside `dir` (one level). A .cue is a
// cue sheet, not a bootable image: when a matching .bin exists, keep the
// .bin and drop the .cue so the game boots from the actual disc image.
static std::vector<fs::path> collect_images(const fs::path &dir) {
  std::vector<fs::path> images;
  std::vector<std::string> cues;
  std::error_code ec;
  for (const auto &inner : fs::directory_iterator(dir, ec)) {
    if (!inner.is_regular_file(ec))
      continue;
    if (!is_pcsx2_image(inner.path()))
      continue;
    if (lower(inner.path().extension().string()) == ".cue")
      cues.push_back(inner.path().stem().string());
    images.push_back(inner.path());
  }
  for (auto it = images.begin(); it != images.end();) {
    if (lower(it->extension().string()) == ".cue") {
      bool hasBin = false;
      for (const auto &f : images)
        if (lower(f.extension().string()) == ".bin" &&
            bin_belongs_to_cue(f, it->stem().string())) {
          hasBin = true;
          break;
        }
      if (hasBin)
        it = images.erase(it);
      else
        ++it;
    } else {
      ++it;
    }
  }
  return images;
}

// Prefer .iso > .img > .elf > .bin > .cue when a folder holds multiple images.
// .cue is last: it is only bootable when no matching .bin exists.
static fs::path prefer_image(const std::vector<fs::path> &images) {
  static const char *pref[] = {".iso", ".img", ".elf", ".bin", ".cue"};
  for (const char *p : pref)
    for (const auto &f : images)
      if (lower(f.extension().string()) == p)
        return f;
  return images.front();
}

// Scan a PCSX2 directory. Each subfolder of `dir` is one game (name = folder
// name); a loose image file directly in `dir` is also a game (name = file name).
static void scan_pcsx2_dir(const fs::path &dir, std::vector<GameEntry> &out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return;

  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) {
      if (!is_pcsx2_image(entry.path()))
        continue;
      // Skip a loose .cue that has a matching .bin next to it; the .bin is
      // the bootable image (PCSX2 does not read cue sheets).
      if (lower(entry.path().extension().string()) == ".cue") {
        bool hasBin = false;
        for (const auto &sib : fs::directory_iterator(dir, ec)) {
          if (sib.is_regular_file(ec) &&
              lower(sib.path().extension().string()) == ".bin" &&
              bin_belongs_to_cue(sib.path(), entry.path().stem().string())) {
            hasBin = true;
            break;
          }
        }
        if (hasBin)
          continue;
      }
      add_game(out, entry.path(), entry.path().stem().string(),
               EmulatorType::PCSX2);
    } else if (entry.is_directory(ec)) {
      std::vector<fs::path> images = collect_images(entry.path());
      if (!images.empty())
        add_game(out, prefer_image(images), entry.path().filename().string(),
                 EmulatorType::PCSX2);
    }
  }
}

// Scan an RPCS3 directory. Each subfolder is one game (name = folder name):
// a jailbreak folder with EBOOT.BIN, or a folder containing a loose .iso.
// A loose .iso directly in `dir` is also a game (name = file name).
static void scan_rpcs3_dir(const fs::path &dir, std::vector<GameEntry> &out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return;

  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) {
      if (lower(entry.path().extension().string()) == ".iso")
        add_game(out, entry.path(), entry.path().stem().string(),
                 EmulatorType::RPCS3);
    } else if (entry.is_directory(ec)) {
      std::string eboot = find_eboot(entry.path());
      if (!eboot.empty()) {
        add_game(out, fs::path(eboot), entry.path().filename().string(),
                 EmulatorType::RPCS3);
        continue;
      }
      std::error_code ec2;
      for (const auto &inner : fs::directory_iterator(entry.path(), ec2)) {
        if (inner.is_regular_file(ec2) &&
            lower(inner.path().extension().string()) == ".iso") {
          add_game(out, inner.path(), entry.path().filename().string(),
                   EmulatorType::RPCS3);
          break;
        }
      }
    }
  }
}

bool load_game_config(const std::string &path, GameConfig &out) {
  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "[Config] Failed to open " << path << "\n";
    return false;
  }

  std::string line;
  std::string section;
  while (std::getline(f, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';')
      continue;
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      section = t.substr(1, t.size() - 2);
      continue;
    }

    // [directories] lines have no '=': "emulator | directory path"
    if (section == "directories" && t.find('=') == std::string::npos) {
      std::vector<std::string> fields = split(t, '|');
      if (fields.size() >= 2 && !fields[0].empty() && !fields[1].empty()) {
        fs::path dir(fields[1]);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
          std::cerr << "[Config] Directory not found: " << dir.string()
                    << "\n";
          continue;
        }
        size_t before = out.games.size();
        if (fields[0] == "rpcs3")
          scan_rpcs3_dir(dir, out.games);
        else
          scan_pcsx2_dir(dir, out.games);
        std::cout << "[Config] Scanned " << dir.string() << " (" << fields[0]
                  << "): " << (out.games.size() - before) << " games\n";
      }
      continue;
    }

    size_t eq = t.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = trim(t.substr(0, eq));
    std::string value = trim(t.substr(eq + 1));

    if (section == "pcsx2" && key == "path") {
      out.pcsx2_path = value;
    } else if (section == "rpcs3" && key == "path") {
      out.rpcs3_path = value;
    } else if (section == "input" && key == "analog") {
      out.analog_input =
          (value == "true" || value == "1" || value == "yes" || value == "on");
    } else if (section == "games") {
      // value = "emulator | boot path | args" (individual game override)
      std::vector<std::string> fields = split(value, '|');
      if (fields.size() >= 2 && !fields[0].empty() && !fields[1].empty()) {
        GameEntry e;
        e.name = key;
        e.emulator = (fields[0] == "rpcs3") ? EmulatorType::RPCS3
                                            : EmulatorType::PCSX2;
        e.boot_path = fields[1];
        if (fields.size() >= 3)
          e.args = fields[2];
        out.games.push_back(std::move(e));
      }
    }
  }

  // Sort by name for a stable, readable list.
  std::sort(out.games.begin(), out.games.end(),
            [](const GameEntry &a, const GameEntry &b) {
              return a.name < b.name;
            });
  return true;
}

} // namespace rps
