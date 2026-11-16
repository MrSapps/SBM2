// plan <snapshot.txt> [goal]: print the goal layer's plan for a world view.
// goal: exit (default), blocks, enemies, items, or several joined with '+'.
// PLAN_NODES=n: the search limit (PlanOptions::maxNodes).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "goap.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: plan <snapshot.txt> [goal]\n");
    return 2;
  }
  sbm2::World w;
  std::string err;
  if (!sbm2::loadWorld(argv[1], w, &err)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
    return 2;
  }
  sbm2::Goal g;
  if (!sbm2::Goal::parse(argc > 2 ? argv[2] : "exit", g, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  auto t0 = std::chrono::steady_clock::now();
  sbm2::PlanOptions po;
  if (const char* n = std::getenv("PLAN_NODES")) po.maxNodes = std::atoi(n);
  sbm2::Plan p = sbm2::plan(w, g, po);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("goal %s: %s, cost %d frames, %d nodes, %.2f ms%s\n", g.str().c_str(), p.ok ? "plan" : "no plan",
              p.cost, p.expanded, ms, p.ok && !p.complete ? " (search limit: greedy tail)" : "");
  if (!p.ok || !p.why.empty()) std::printf("%s\n", p.why.c_str());
  std::printf("%s", p.str().c_str());
  return p.ok ? 0 : 1;
}
