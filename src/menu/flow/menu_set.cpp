#include "menu/flow/menu_set.h"

#include "base/log.h"
#include "vfs/vfs.h"

namespace gt3r::menu::flow {

FileReader vfs_reader(const Vfs& vfs) {
    return [&vfs](const std::string& path) { return vfs.read(path); };
}

MenuPatches& menu_patches() {
    static MenuPatches p;
    return p;
}

MenuSet::MenuSet(FileReader reader, std::string name, std::string lang)
    : reader_(std::move(reader)), name_(std::move(name)), lang_(std::move(lang)) {
    std::string path = "menu/" + name_ + ".mbl";
    auto data = reader_(path);
    if (!data) throw FormatError("cannot read " + path);
    mbl_ = MblFile::parse(*data);
    if (menu_patches().flow) menu_patches().flow(name_, reader_, mbl_);
}

std::string MenuSet::pmb_path(const std::string& lang, const std::string& name, int n) {
    return "menu/" + lang + "/" + name + std::to_string(n) + ".pmb";
}

int MenuSet::page_pmb(const MblFile& flow, std::string_view page) {
    const int fp = flow.find_page(page);
    const int n = fp >= 0 ? pmb_number(flow.pages[size_t(fp)]) : 1;
    return n <= 0 ? 1 : n;
}

std::unique_ptr<MenuScene> MenuSet::load_scene(const FileReader& reader, const std::string& path) {
    try {
        if (auto data = reader(path)) return std::make_unique<MenuScene>(MenuScene::load(*data, path));
        LOG_WARN("menu: cannot load [%s]", path.c_str());
    } catch (const std::exception& e) {
        LOG_WARN("menu: %s: %s", path.c_str(), e.what());
    }
    return nullptr;
}

void MenuSet::adopt(int n, std::unique_ptr<MenuScene> s) const {
    if (scenes_.count(n)) return;
    try {
        if (s && menu_patches().layout) menu_patches().layout(name_, n, reader_, *s);
    } catch (const std::exception& e) {
        LOG_WARN("menu: %s: %s", pmb_path(n).c_str(), e.what());
        s.reset();
    }
    scenes_[n] = std::move(s);
}

const MenuScene* MenuSet::scene(int n) const {
    auto it = scenes_.find(n);
    if (it != scenes_.end()) return it->second.get();
    adopt(n, load_scene(reader_, pmb_path(n)));
    return scenes_[n].get();
}

MenuSet::PageRef MenuSet::page(std::string_view name) const {
    PageRef r;
    int fp = mbl_.find_page(name);
    if (fp >= 0) r.flow = &mbl_.pages[size_t(fp)];
    r.scene = scene(page_pmb(mbl_, name));
    if (r.scene) r.page = r.scene->file().find_page(name);
    return r;
}

} // namespace gt3r::menu::flow
