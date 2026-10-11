#pragma once

#include "engine/asset/asset_view.hpp"

// Fresh asset identities. A GUID is 128 random bits; the null GUID is never
// returned. Seeded once per process from the OS, so two runs never repeat.
namespace GUID {

AssetGuid random();

} // namespace GUID
