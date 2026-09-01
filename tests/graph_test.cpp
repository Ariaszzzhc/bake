import std;
import bake.buildsystem.graph;
import bake.buildsystem.moid;

namespace {

bake::MoidNode make_node(std::string id) {
    bake::MoidNode node;
    node.id = bake::MoidId{id};
    node.declaration.id = id;
    node.declaration.name = id;
    return node;
}

bool reports_orphan(const bake::MoidGraph& graph, std::string_view id) {
    auto result = bake::topological_moids(graph);
    if (result) {
        std::println(std::cerr,
                     "topological_moids accepted orphan '{}'", id);
        return false;
    }
    if (result.error().find("orphan moid '" + std::string(id) + "'") ==
            std::string::npos ||
        result.error().find("not reachable from any graph root") ==
            std::string::npos) {
        std::println(std::cerr,
                     "unexpected orphan diagnostic: {}", result.error());
        return false;
    }
    return true;
}

} // namespace

int main() {
    bake::MoidGraph graph;
    graph.nodes.emplace(bake::MoidId{"root"}, make_node("root"));
    graph.nodes.emplace(bake::MoidId{"orphan"}, make_node("orphan"));
    graph.roots.push_back(bake::MoidId{"root"});
    if (!reports_orphan(graph, "orphan")) return 1;

    bake::MoidGraph rootless;
    rootless.nodes.emplace(bake::MoidId{"rootless"}, make_node("rootless"));
    if (!reports_orphan(rootless, "rootless")) return 1;

    return 0;
}
