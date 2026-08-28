#include <windows.h>
#include <stdio.h>
#include "vulkan/vulkan.h"

typedef VkResult (*PFN_vkCreateInstance_)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult (*PFN_vkEnumeratePhysicalDeviceGroups_)(VkInstance, uint32_t*, VkPhysicalDeviceGroupProperties*);
typedef void*    (*PFN_vkGetInstanceProcAddr_)(VkInstance, const char*);

int main(void) {
    HMODULE hVulkan = LoadLibraryA("vulkan-1.dll");
    if (!hVulkan) { printf("FAILED to load vulkan-1.dll\n"); return 1; }

    PFN_vkGetInstanceProcAddr_ gipa =
        (PFN_vkGetInstanceProcAddr_)GetProcAddress(hVulkan, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance_ createInstance =
        (PFN_vkCreateInstance_)gipa(NULL, "vkCreateInstance");

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "devgroup_test";
    appInfo.apiVersion = VK_API_VERSION_1_1; // device groups need >=1.1

    const char* extNames[] = { "VK_KHR_device_group_creation" };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = extNames;

    VkInstance instance;
    VkResult vr = createInstance(&ici, NULL, &instance);
    if (vr != VK_SUCCESS) { printf("vkCreateInstance failed: %d\n", vr); return 1; }

    PFN_vkEnumeratePhysicalDeviceGroups_ enumGroups =
        (PFN_vkEnumeratePhysicalDeviceGroups_)gipa(instance, "vkEnumeratePhysicalDeviceGroups");
    if (!enumGroups) { printf("vkEnumeratePhysicalDeviceGroups not found\n"); return 1; }

    uint32_t groupCount = 0;
    enumGroups(instance, &groupCount, NULL);
    printf("Device group count: %u\n", groupCount);

    VkPhysicalDeviceGroupProperties* groups =
        (VkPhysicalDeviceGroupProperties*)calloc(groupCount, sizeof(VkPhysicalDeviceGroupProperties));
    for (uint32_t i = 0; i < groupCount; i++)
        groups[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;

    enumGroups(instance, &groupCount, groups);

    for (uint32_t i = 0; i < groupCount; i++) {
        printf("Group %u: physicalDeviceCount = %u, subsetAllocation = %d\n",
            i, groups[i].physicalDeviceCount, groups[i].subsetAllocation);
    }

    return 0;
}
