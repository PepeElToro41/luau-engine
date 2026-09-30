#pragma once

#include <vulkan/vulkan.h>

namespace engine {

class Engine {
public:
    void init();
    void run();
    void shutdown();

private:
    VkInstance m_instance = VK_NULL_HANDLE;
};

} // namespace engine
