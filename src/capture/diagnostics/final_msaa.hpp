#pragma once
#include "command_stream.hpp"
#include <filesystem>

namespace flora::capture::diagnostics {
// Development-only: called on the capture owner thread after recording ends.
// The directory is separate from the GPA transaction and never establishes
// publication or acceptance. No encoder record IDs or replay objects are used.
void export_final_msaa(ID3D11DeviceContext*, const RecordedFrame&,
                       const std::filesystem::path&, const SnapshotOptions& = {});
}
