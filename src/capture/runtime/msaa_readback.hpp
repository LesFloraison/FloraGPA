#pragma once
#include "msaa_samples.hpp"
#include "resource_data.hpp"

namespace flora::capture {
// Exclusive, synchronized application-context boundary; tracked objects only.
// Owns CPU data on return. Internal compute work is isolated in a native command
// list, with immediate state restoration. Active queries/predicates reject.
// This primitive does not enable MSAA file publication or bypass encoder guards.
MsaaSamples snapshot_msaa_samples(ID3D11DeviceContext*, ID3D11Texture2D*,
                                 const SnapshotOptions& options={});
}
