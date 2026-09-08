#include "app/NoDocumentCanvas.hpp"

namespace np {

bool releaseSolverWhenNoDocuments(std::unique_ptr<PaintSim>& sim,
                                   const DocumentSession& documents) {
  if (!sim || !documents.empty()) return false;
  sim->shutdown();
  sim.reset();
  return true;
}

}  // namespace np
