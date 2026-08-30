// extmem_test.c -- minimal: do these two GPUs have matching device identity?
//
// Opaque Vulkan external memory (OPAQUE_WIN32) requires the importing device
// to have the same deviceUUID and driverUUID as the exporting device. Two
// distinct physical cards normally differ, which would make direct
// cross-device image sharing spec-illegal. This checks.
//
// Build (MSYS2 MinGW64):
//   gcc -std=c11 -O2 -I ~/dxvk/include/vulkan/include -o extmem_test.exe extmem_test.c

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr          gpa;
static PFN_vkEnumeratePhysicalDevices     enumDevs;
static PFN_vkGetPhysicalDeviceProperties2 getProps2;
static PFN_vkEnumerateDeviceExtensionProperties enumExts;

static void putUuid(const char *label, const uint8_t *u) {
    printf("    %-12s", label);
    for (int i = 0; i < VK_UUID_SIZE; i++) printf("%02x", u[i]);
    printf("\n");
}

static int hasExt(VkPhysicalDevice d, const char *name) {
    uint32_t n = 0;
    enumExts(d, NULL, &n, NULL);
    if (!n) return 0;
    VkExtensionProperties *p = malloc(n * sizeof(*p));
    enumExts(d, NULL, &n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name)) { found = 1; break; }
    free(p);
    return found;
}

int main(void) {
    HMODULE dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) { printf("no vulkan-1.dll\n"); return 1; }

    gpa = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance createInst =
        (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;

    VkInstance inst;
    if (createInst(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n"); return 1;
    }

    enumDevs  = (PFN_vkEnumeratePhysicalDevices)gpa(inst, "vkEnumeratePhysicalDevices");
    getProps2 = (PFN_vkGetPhysicalDeviceProperties2)gpa(inst, "vkGetPhysicalDeviceProperties2");
    enumExts  = (PFN_vkEnumerateDeviceExtensionProperties)gpa(inst, "vkEnumerateDeviceExtensionProperties");

    uint32_t count = 0;
    enumDevs(inst, &count, NULL);
    printf("Physical devices: %u\n\n", count);
    if (count < 1) return 1;

    VkPhysicalDevice *devs = malloc(count * sizeof(*devs));
    enumDevs(inst, &count, devs);

    uint8_t devUuid[8][VK_UUID_SIZE], drvUuid[8][VK_UUID_SIZE];

    for (uint32_t i = 0; i < count && i < 8; i++) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &id;
        getProps2(devs[i], &p2);

        memcpy(devUuid[i], id.deviceUUID, VK_UUID_SIZE);
        memcpy(drvUuid[i], id.driverUUID, VK_UUID_SIZE);

        printf("Device %u: %s\n", i, p2.properties.deviceName);
        putUuid("deviceUUID", id.deviceUUID);
        putUuid("driverUUID", id.driverUUID);
        printf("    ext_mem_win32  %s\n", hasExt(devs[i], "VK_KHR_external_memory_win32") ? "yes" : "NO");
        printf("    ext_sem_win32  %s\n", hasExt(devs[i], "VK_KHR_external_semaphore_win32") ? "yes" : "NO");
        printf("    ext_mem_host   %s\n", hasExt(devs[i], "VK_EXT_external_memory_host") ? "yes" : "NO");
        printf("\n");
    }

    if (count >= 2) {
        int sameDev = !memcmp(devUuid[0], devUuid[1], VK_UUID_SIZE);
        int sameDrv = !memcmp(drvUuid[0], drvUuid[1], VK_UUID_SIZE);
        printf("=== deviceUUID match: %s   driverUUID match: %s ===\n",
            sameDev ? "YES" : "NO", sameDrv ? "YES" : "NO");
        printf("%s\n", (sameDev && sameDrv)
            ? "Opaque cross-device sharing is permitted by identity rules."
            : "Opaque cross-device sharing NOT permitted; need a host-memory path.");
    }

    return 0;
}
