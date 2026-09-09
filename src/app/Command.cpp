#include "app/Command.hpp"

#include "app/CommandSupport.hpp"

namespace np {
namespace {

const std::vector<CommandSpec>& table() {
  static const std::vector<CommandSpec> kTable = [] {
    std::vector<CommandSpec> rows;
    // The order here is the order the ACTIONS panel lists commands in, and
    // nothing else depends on it: every lookup is by id. Each family appends
    // from its own translation unit -- see app/Command.hpp's note on why they
    // are separate files.
    registerLayerCommandRows(&rows);
    registerImageCommands(&rows);
    registerOpStackCommands(&rows);
    registerPatternCommands(&rows);
    return rows;
  }();
  return kTable;
}

}  // namespace

size_t layerIndexNamed(const Document& doc, std::string_view name) {
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (doc.layers[i].name == name) return i;
  return doc.layers.size();
}

const std::vector<CommandSpec>& allCommands() { return table(); }

const CommandSpec* findCommand(std::string_view id) {
  for (const CommandSpec& spec : table())
    if (id == spec.id) return &spec;
  return nullptr;
}

CommandResult applyCommand(OpenDocument& doc, const Command& command) {
  const CommandSpec* spec = findCommand(command.id);
  if (spec == nullptr) {
    return commandRefused("refused: this build has no command called \"" + command.id +
                          "\". An action written by a newer build is refused rather than run "
                          "with the step skipped, because a skipped step writes a file that "
                          "looks correct and is not.");
  }
  const std::string unavailable = spec->unavailableReason(doc, command.params);
  if (!unavailable.empty()) return commandRefused(unavailable);
  return spec->apply(doc, command.params);
}

}  // namespace np
