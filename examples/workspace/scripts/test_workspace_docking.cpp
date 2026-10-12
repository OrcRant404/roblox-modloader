// Run from the project root. No Qt library or Studio process is loaded; the
// linker removes unused native entry points. The default is the preserved A XML
// fixture below; an optional argument can select another captured A snapshot:
// /usr/bin/clang++ -std=c++23 -O2 -ffunction-sections -fdata-sections -Wl,-dead_strip \
//   -Wall -Wextra -Wpedantic -Werror -Wno-invalid-constexpr \
//   -I build/workspace/include \
//   -I "$RML_SOURCE/code/roblox_modloader/include" \
//   -I /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/libxml2 \
//   scripts/test_workspace_docking.cpp -lxml2 -o /private/tmp/workspace-docking-test
// /private/tmp/workspace-docking-test [scripts/test-fixtures/workspace-a-layout.xml]
#include "../native-workspace/docking.cpp"
#include <libxml/c14n.h>
#include <iostream>
#include <limits>
#include <cstdlib>

namespace {
std::shared_ptr<SplitNode> leaf(std::string id) {
    auto node = std::make_shared<SplitNode>();
    node->panel_id = std::move(id);
    return node;
}
std::shared_ptr<SplitNode> split(Direction direction, double scale,
    std::shared_ptr<const SplitNode> first, std::shared_ptr<const SplitNode> second, int offset = 0) {
    auto node = std::make_shared<SplitNode>();
    node->direction = direction;
    node->size = {scale, offset};
    node->first = std::move(first);
    node->second = std::move(second);
    return node;
}
NativePanel panel(std::string id, int width, int height, bool document = false) {
    NativePanel result;
    result.info.id = std::move(id);
    result.info.minimum_width = width;
    result.info.minimum_height = height;
    result.info.document = document;
    return result;
}
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
}
template<class F> void rejects(F&& operation, const char* name) {
    bool rejected = false;
    try { operation(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, name);
}
std::unique_ptr<PreparedNode> prepared(const SplitNode& root, const std::vector<NativePanel>& panels) {
    std::set<std::string> ids;
    std::set<const SplitNode*> ancestry;
    return prepare_node(root, panels, ids, ancestry, 0);
}

using TestDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
TestDocument parse(const std::string& bytes) {
    TestDocument doc(xmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), "test.xml", nullptr,
        XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    check(static_cast<bool>(doc), "test fixture XML parses");
    return doc;
}
std::map<std::string, xmlNode*> declarations(xmlDoc* doc) {
    std::map<std::string, xmlNode*> result;
    for (auto* section = xmlDocGetRootElement(doc)->children; section; section = section->next)
        if (xml_named(section, "Panels")) for (auto* node = section->children; node; node = node->next)
            if (xml_named(node, "Panel")) result.emplace(xml_attribute(node, "ID"), node);
    return result;
}
xmlNode* child_panels(xmlNode* node) {
    for (auto* child = node->children; child; child = child->next)
        if (xml_named(child, "Panels")) return child;
    return nullptr;
}
std::string dump(xmlDoc* doc) {
    xmlChar* bytes = nullptr;
    int length = 0;
    xmlDocDumpFormatMemoryEnc(doc, &bytes, &length, "UTF-8", 1);
    check(bytes && length > 0, "test XML serializes");
    std::string result(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    xmlFree(bytes);
    return result;
}
std::string canonical(xmlDoc* doc) {
    xmlChar* bytes = nullptr;
    const int length = xmlC14NDocDumpMemory(doc, nullptr, XML_C14N_1_0, nullptr, 0, &bytes);
    check(bytes && length > 0, "test XML canonicalizes");
    std::string result(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    xmlFree(bytes);
    return result;
}
void geometry(NativeBranch& branch, int x, int y, int width, int height, bool document = false) {
    branch.geometry.id = branch.id;
    branch.geometry.x = x; branch.geometry.y = y;
    branch.geometry.width = width; branch.geometry.height = height;
    branch.geometry.document = document;
    branch.document_tab_height = document ? 26 : 0;
}
void xml_tests(const std::filesystem::path& fixture) {
    const auto original = read_snapshot(fixture);
    auto fixture_doc = parse(original);
    const auto original_declarations = declarations(fixture_doc.get());
    std::string aid;
    std::map<std::string, std::string> parent_ids;
    for (const auto& [id, node] : original_declarations) {
        if (id.ends_with("workspace-test.A.Panel")) aid = id;
        if (auto* children = child_panels(node)) for (auto* ref = children->children; ref; ref = ref->next)
            if (xml_named(ref, "Panel")) check(parent_ids.emplace(xml_attribute(ref, "ID"), id).second,
                "captured A has unique active child parents");
    }
    check(!aid.empty() && original_declarations.contains(std::string(kViewportId)), "captured A panel IDs exist");
    const auto widget_parent = parent_ids.at(aid);
    const auto doc_parent = parent_ids.at(std::string(kViewportId));
    check(parent_ids.at(widget_parent) == parent_ids.at(doc_parent), "captured A sibling containers share a split");
    const auto split_id = parent_ids.at(widget_parent);
    const LeafSet included{aid, std::string(kViewportId)};
    std::map<std::string, NativeBranch> native;
    for (const auto& leaf : included) {
        std::string current = leaf;
        while (!current.empty()) {
            NativeBranch branch;
            branch.id = current;
            branch.parent_id = parent_ids.contains(current) ? parent_ids.at(current) : std::string{};
            branch.class_name = xml_attribute(original_declarations.at(current), "Type");
            geometry(branch, 0, 95, 1280, 725);
            native.emplace(current, std::move(branch));
            current = native.at(current).parent_id;
        }
    }
    // Geometry from the overflowing live A run. The document tab strip is 26px;
    // the ordinary panel has the observed one-pixel frame difference.
    geometry(native.at(aid), 0, 95, 804, 726);
    geometry(native.at(widget_parent), 0, 95, 804, 725);
    geometry(native.at(doc_parent), 807, 95, 1280, 725);
    geometry(native.at(std::string(kViewportId)), 807, 121, 1280, 699, true);
    geometry(native.at(split_id), 0, 95, 2087, 725);
    const auto tree = split(Direction::Horizontal, 0.40, leaf(aid), leaf("Viewport"));
    auto plan = prepared(*tree, {panel(aid, 100, 123), panel(std::string(kViewportId), 300, 100, true)});
    allocate(*plan, 1280, 725);
    char temporary[] = "/private/tmp/workspace-docking-test-XXXXXX";
    const auto* directory = mkdtemp(temporary);
    check(directory, "test artifact directory created");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    unsigned sequence = 0;
    const auto source = [&](const std::string& bytes) {
        auto path = cleanup.path / ("snapshot-" + std::to_string(sequence++) + ".xml");
        write_snapshot(path, bytes);
        return path;
    };
    const auto sized = size_snapshot(source(original), *plan, included, native);
    check(sized.splits.size() == 1 && sized.splits.front().id == split_id,
        "captured A physical split maps to descriptor");
    auto output = parse(read_snapshot(sized.path));
    const auto patched = declarations(output.get());
    check(patched.size() == original_declarations.size(), "all captured declarations preserved");
    for (const auto& [id, branch] : native) {
        const auto* node = patched.at(id);
        const bool first = id == aid || id == widget_parent;
        const bool second = id == kViewportId || id == doc_parent;
        check(xml_attribute(node, "DockCX") == (first ? "512" : second ? "768" : "1280"),
            "both branches and every ancestor receive explicit width hints");
        check(xml_attribute(node, "DockCY") == (id == kViewportId ? "699" : "725"),
            "document content excludes its measured tab strip");
        (void)branch;
        for (const auto* attr : {"DockCX", "DockCY"}) {
            const auto before = xml_attribute(original_declarations.at(id), attr);
            xmlSetProp(patched.at(id), BAD_CAST attr, BAD_CAST before.c_str());
        }
    }
    check(canonical(output.get()) == canonical(fixture_doc.get()),
        "only intended DockCX/DockCY attributes change; graph and hidden panels preserved");
    auto corrected = native;
    for (auto& [id, branch] : corrected) { (void)id; geometry(branch, 0, 95, 1280, 725); }
    geometry(corrected.at(aid), 0, 95, 511, 726);
    geometry(corrected.at(widget_parent), 0, 95, 511, 725);
    geometry(corrected.at(doc_parent), 514, 95, 766, 725);
    geometry(corrected.at(std::string(kViewportId)), 514, 121, 766, 699, true);
    PanelInfo managed;
    managed.x = 0; managed.y = 95; managed.width = 1280; managed.height = 725;
    check(!verify_splits(sized.splits, corrected, included, managed).empty(), "40 percent split accepts native splitter rounding");
    auto unsettled_tabs = corrected;
    unsettled_tabs.at(std::string(kViewportId)).document_tab_height = 0;
    rejects([&] { (void)verify_splits(sized.splits, unsettled_tabs, included, managed); },
        "unsettled document tabs cannot pass even with full-sized leaves");
    rejects([&] { (void)size_snapshot(source(original), *plan, included, unsettled_tabs); },
        "unsettled document tab height cannot supply XML content hints");
    auto default_leaves = corrected;
    geometry(default_leaves.at(aid), 0, 95, 100, 123);
    geometry(default_leaves.at(std::string(kViewportId)), 0, 95, 640, 480, true);
    default_leaves.at(std::string(kViewportId)).document_tab_height = 0;
    rejects([&] { (void)verify_splits(sized.splits, default_leaves, included, managed); },
        "observed post-restore default leaf geometry remains rejected");
    auto edge_tolerance = corrected;
    edge_tolerance.at(std::string(kViewportId)).document_tab_height = 25;
    check(!verify_splits(sized.splits, edge_tolerance, included, managed).empty(),
        "one pixel at each aligned edge accepts a two-pixel height difference");
    rejects([&] { (void)verify_splits(sized.splits, native, included, managed); }, "observed overflow cannot pass allocation validation");
    try {
        (void)verify_splits(sized.splits, native, included, managed);
        check(false, "overflow produces rectangle diagnostics");
    } catch (const std::runtime_error& error) {
        const std::string diagnostic = error.what();
        check(diagnostic.find("role=parent") != std::string::npos
            && diagnostic.find("role=first") != std::string::npos
            && diagnostic.find("role=second") != std::string::npos
            && diagnostic.find("width=2087") != std::string::npos
            && diagnostic.find("measurement=QtWidgets.QWidget.metric") != std::string::npos,
            "rejected split reports all three measured rectangles without native addresses");
    }
    auto wrong_ratio = corrected;
    geometry(wrong_ratio.at(aid), 0, 95, 900, 726);
    geometry(wrong_ratio.at(widget_parent), 0, 95, 900, 725);
    geometry(wrong_ratio.at(doc_parent), 903, 95, 377, 725);
    geometry(wrong_ratio.at(std::string(kViewportId)), 903, 121, 377, 699, true);
    rejects([&] { (void)verify_splits(sized.splits, wrong_ratio, included, managed); }, "contained but wrong ratio rejected");
    auto virtual_containers = corrected;
    for (auto& [id, branch] : virtual_containers) if (!included.contains(id)) geometry(branch, 0, 95, 0, 0);
    geometry(virtual_containers.at(widget_parent), 0, 95, 100, 30);
    geometry(virtual_containers.at(doc_parent), 0, 95, 640, 480);
    check(!verify_splits(sized.splits, virtual_containers, included, managed).empty(),
        "real leaves verify with the exact observed virtual layout placeholders");
    const auto resized_virtual = size_snapshot(source(original), *plan, included, virtual_containers);
    const auto virtual_doc = parse(read_snapshot(resized_virtual.path));
    check(xml_attribute(declarations(virtual_doc.get()).at(std::string(kViewportId)), "DockCY") == "699",
        "document hint uses real painted tab height independently of virtual parent rectangle");
    auto incomplete = corrected;
    geometry(incomplete.at(aid), 0, 95, 200, 726);
    geometry(incomplete.at(std::string(kViewportId)), 203, 121, 300, 699, true);
    rejects([&] { (void)verify_splits(sized.splits, incomplete, included, managed); },
        "proportioned visible leaves must still fill the actual managed widget");
    const auto mutated = [&](auto mutate) {
        auto doc = parse(original);
        mutate(doc.get());
        return source(dump(doc.get()));
    };
    rejects([&] { (void)size_snapshot(source("not XML"), *plan, included, native); }, "malformed XML rejected");
    rejects([&] {
        const auto path = mutated([](xmlDoc* doc) { xmlSetProp(xmlDocGetRootElement(doc), BAD_CAST "version", BAD_CAST "9.9"); });
        (void)size_snapshot(path, *plan, included, native);
    }, "unsupported XML version rejected");
    rejects([&] {
        const auto path = mutated([&](xmlDoc* doc) {
            const auto nodes = declarations(doc);
            xmlAddSibling(nodes.at(aid), xmlCopyNode(nodes.at(aid), 1));
        });
        (void)size_snapshot(path, *plan, included, native);
    }, "duplicate XML panel ID rejected");
    rejects([&] {
        const auto path = mutated([&](xmlDoc* doc) {
            auto* node = declarations(doc).at(doc_parent);
            xmlUnlinkNode(node); xmlFreeNode(node);
        });
        (void)size_snapshot(path, *plan, included, native);
    }, "missing branch declaration rejected");
    rejects([&] {
        const auto path = mutated([&](xmlDoc* doc) { xmlSetProp(child_panels(declarations(doc).at(widget_parent)), BAD_CAST "Count", BAD_CAST "0"); });
        (void)size_snapshot(path, *plan, included, native);
    }, "child count mismatch rejected");
    rejects([&] {
        const auto path = mutated([&](xmlDoc* doc) {
            xmlSetProp(declarations(doc).at(split_id), BAD_CAST "Horizontal", BAD_CAST "False");
        });
        (void)size_snapshot(path, *plan, included, native);
    }, "wrong physical split direction rejected");
    rejects([&] {
        const auto path = mutated([&](xmlDoc* doc) {
            auto* children = child_panels(declarations(doc).at(split_id));
            auto* first = children->children;
            xmlUnlinkNode(first); xmlAddChild(children, first);
        });
        (void)size_snapshot(path, *plan, included, native);
    }, "reversed physical First/Second order rejected");
    auto mismatched = native;
    mismatched.at(aid).parent_id = doc_parent;
    rejects([&] { (void)size_snapshot(source(original), *plan, included, mismatched); }, "native/XML parent disagreement rejected");
    auto bad_inset = native;
    bad_inset.at(std::string(kViewportId)).document_tab_height = 129;
    rejects([&] { (void)size_snapshot(source(original), *plan, included, bad_inset); }, "unsupported document tab height rejected");
    rejects([&] { write_snapshot(sized.path, original); }, "existing sized artifact cannot be overwritten");
    std::cout << "PASS: captured A XML size hints, graph preservation, parent mapping, split proportions, malformed input rejection\n";
}

void nested_rendered_tests() {
    std::map<std::string, NativeBranch> branches;
    for (const auto* id : {"root", "left-layout", "right-layout", "doc-layout", "bottom-layout", "Left", "Viewport", "Bottom"}) {
        NativeBranch branch;
        branch.id = id;
        geometry(branch, 0, 95, 0, 0);
        branches.emplace(id, branch);
    }
    geometry(branches.at("Left"), 0, 95, 511, 726);
    geometry(branches.at("Viewport"), 514, 121, 766, 408, true);
    geometry(branches.at("Bottom"), 514, 532, 766, 288);
    const std::vector<NativeSplit> splits{
        {"root", "left-layout", "right-layout", Direction::Horizontal, 0.40, 0, {"Left"}, {"Viewport", "Bottom"}},
        {"right-layout", "doc-layout", "bottom-layout", Direction::Vertical, 0.60, 0, {"Viewport"}, {"Bottom"}},
    };
    PanelInfo managed;
    managed.x = 0; managed.y = 95; managed.width = 1280; managed.height = 725;
    const LeafSet included{"Left", "Viewport", "Bottom"};
    check(!verify_splits(splits, branches, included, managed).empty(),
        "nested horizontal and vertical allocations use real leaves plus document header");
    auto edges = branches;
    edges.at("Bottom").geometry.x += 1;
    edges.at("Bottom").geometry.width -= 2;
    check(!verify_splits(splits, edges, included, managed).empty(),
        "vertical split accepts one pixel per edge without widening allocation tolerance");
    auto misaligned = branches;
    misaligned.at("Bottom").geometry.x += 8;
    misaligned.at("Bottom").geometry.width -= 8;
    rejects([&] { (void)verify_splits(splits, misaligned, included, managed); },
        "nested branch cross-axis misalignment rejected");
    std::cout << "PASS: visible-leaf unions, real tab height, nested splits, virtual-container independence\n";
}
}
int main(int argc, char** argv) {
    const std::vector<NativePanel> panels{
        panel("edit_user_unique_Graph", 320, 240),
        panel("edit_user_unique_CustomProperties", 200, 160),
        panel("ideDocDocumentPanel", 1, 1, true),
        panel("outputWidgetPanel", 100, 100),
    };
    check(resolve(panels, "edit_user_unique_Graph") == "edit_user_unique_Graph", "exact ID");
    check(resolve(panels, "Graph") == "edit_user_unique_Graph", "unique widget suffix");
    check(resolve(panels, "Viewport") == "ideDocDocumentPanel", "Viewport alias");
    check(resolve(panels, "Output") == "outputWidgetPanel", "Output alias");
    rejects([&] { (void)resolve(panels, "raph"); }, "suffix boundary required");
    auto ambiguous = panels;
    ambiguous.push_back(panel("edit_user_other_Graph", 320, 240));
    rejects([&] { (void)resolve(ambiguous, "Graph"); }, "ambiguous suffix rejected");
    auto tree = split(Direction::Horizontal, 0.70, leaf("Graph"),
        split(Direction::Vertical, 0.65, leaf("Viewport"), leaf("CustomProperties")));
    auto plan = prepared(*tree, panels);
    allocate(*plan, 1280, 900);
    check(plan->first->width == 896 && plan->first->height == 900, "first horizontal allocation");
    check(plan->second->first->width == 384 && plan->second->first->height == 585, "nested first allocation");
    check(plan->second->second->height == 315, "nested remainder allocation");
    auto offset = split(Direction::Horizontal, 0.5, leaf("Graph"), leaf("Viewport"), 17);
    auto offset_plan = prepared(*offset, panels);
    allocate(*offset_plan, 1280, 900);
    check(offset_plan->first->width == 657 && offset_plan->second->width == 623, "UDim offset applied");
    auto duplicate = split(Direction::Horizontal, 0.5, leaf("Graph"), leaf("edit_user_unique_Graph"));
    rejects([&] { prepared(*duplicate, panels); }, "resolved duplicate IDs rejected");
    auto malformed = leaf("Graph");
    malformed->first = leaf("Viewport");
    rejects([&] { prepared(*malformed, panels); }, "mixed leaf/split rejected");
    auto invalid_scale = split(Direction::Horizontal, std::numeric_limits<double>::quiet_NaN(), leaf("Graph"), leaf("Viewport"));
    rejects([&] { prepared(*invalid_scale, panels); }, "nonfinite scale rejected");
    auto impossible = split(Direction::Horizontal, 0.99, leaf("Graph"), leaf("CustomProperties"));
    auto impossible_plan = prepared(*impossible, panels);
    rejects([&] { allocate(*impossible_plan, 1280, 900); }, "minimum size checked before mutation");
    auto no_remainder = split(Direction::Horizontal, 0.5, leaf("Graph"), leaf("Viewport"), 1280);
    auto remainder_plan = prepared(*no_remainder, panels);
    rejects([&] { allocate(*remainder_plan, 1280, 900); }, "offset without remainder rejected");
    auto cycle = std::make_shared<SplitNode>();
    cycle->first = cycle;
    cycle->second = leaf("Viewport");
    rejects([&] { prepared(*cycle, panels); }, "cyclic descriptor rejected");
    cycle->first.reset();
    auto vertical = split(Direction::Vertical, 0.5, leaf("Wide1"), leaf("Wide2"));
    auto vertical_plan = prepared(*vertical, {panel("Wide1", 20000, 100), panel("Wide2", 20000, 100)});
    allocate(*vertical_plan, 25000, 900);
    check(vertical_plan->minimum_width == 20000, "vertical split uses max width, not sum");
    PanelInfo bounds;
    bounds.x = 0; bounds.y = 95; bounds.width = 1280; bounds.height = 725;
    PanelInfo measured;
    measured.x = 807; measured.y = 121; measured.width = 1280; measured.height = 699;
    check(!is_contained(measured, bounds), "reported live viewport overflow rejected");
    measured.x = 515; measured.width = 765;
    check(is_contained(measured, bounds), "viewport touching right/bottom bounds accepted");
    measured.x = -1;
    check(is_contained(measured, bounds), "one-pixel frame tolerance accepted");
    measured.x = -2;
    check(!is_contained(measured, bounds), "left overflow rejected");
    measured.x = 515; measured.height = 700;
    check(is_contained(measured, bounds), "one-pixel bottom frame tolerance accepted");
    measured.height = 701;
    check(!is_contained(measured, bounds), "bottom overflow rejected");
    measured.height = 0;
    check(!is_contained(measured, bounds), "zero extent rejected");
    std::cout << "PASS: ID resolution, nested UDim allocation, minimum sizes, invalid trees, cycle rejection, panel containment\n";
    check(argc <= 2, "usage: workspace-docking-test [captured-A.xml]");
    xml_tests(argc == 2 ? argv[1] : "scripts/test-fixtures/workspace-a-layout.xml");
    nested_rendered_tests();
}
