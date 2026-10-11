#include "engine/utils/guid.hpp"

#include <random>

AssetGuid GUID::random() {
    static std::mt19937_64 generator{std::random_device{}()};
    AssetGuid guid;
    do {
        guid.lo = generator();
        guid.hi = generator();
    } while (guid.is_null());
    return guid;
}
