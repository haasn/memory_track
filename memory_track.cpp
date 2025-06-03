#include "vulkan.h"
#include "vk_layer.h"

#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <cstdio>
#include <vector>

#include <mutex>
#include <map>

#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
  return *(void **)inst;
}

// layer book-keeping information, to store dispatch tables by key
std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
std::map<void *, VkLayerDispatchTable> device_dispatch;

// actual data we're recording in this layer
struct MemoryTypeInfo
{
  VkMemoryType memoryType;
  uint64_t currentUsage;
  uint64_t maximumUsage;
};

struct MemoryHeapInfo
{
  VkMemoryHeap memoryHeap;
  uint64_t currentUsage;
  uint64_t maximumUsage;
};

struct DeviceStats
{
    std::vector<MemoryTypeInfo> memoryTypes;
    std::vector<MemoryHeapInfo> memoryHeaps;
};

std::map<VkDevice, struct DeviceStats> devices;

// keep track of all allocations so we can properly account them on free
// note that this does not perform a deep copy, so pNext chains are invalid
std::map<VkDeviceMemory, VkMemoryAllocateInfo> allocations;

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);
  if (ret == VK_SUCCESS)
  {

    // fetch our own dispatch table for the functions we need, into the next layer
    VkLayerInstanceDispatchTable dispatchTable;
    dispatchTable.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gpa(*pInstance, "vkGetInstanceProcAddr");
    dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)gpa(*pInstance, "vkDestroyInstance");
    dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gpa(*pInstance, "vkEnumerateDeviceExtensionProperties");

    // store the table by key
    {
        scoped_lock l(global_lock);
        instance_dispatch[GetKey(*pInstance)] = dispatchTable;
    }

  }

  return ret;
}

VK_LAYER_EXPORT void VKAPI_CALL MemoryTrack_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  instance_dispatch.erase(GetKey(instance));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  if (ret == VK_SUCCESS)
  {

    // fetch our own dispatch table for the functions we need, into the next layer
    VkLayerDispatchTable dispatchTable;
    dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
    dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
    dispatchTable.AllocateMemory = (PFN_vkAllocateMemory)gdpa(*pDevice, "vkAllocateMemory");
    dispatchTable.FreeMemory = (PFN_vkFreeMemory)gdpa(*pDevice, "vkFreeMemory");

    // store the table by key
    {
        scoped_lock l(global_lock);
        device_dispatch[GetKey(*pDevice)] = dispatchTable;
    }

    VkPhysicalDeviceMemoryProperties memoryProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");

    struct DeviceStats deviceStats = {};
    deviceStats.memoryTypes.resize(memoryProperties.memoryTypeCount);
    deviceStats.memoryHeaps.resize(memoryProperties.memoryHeapCount);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
      deviceStats.memoryTypes[i].memoryType = memoryProperties.memoryTypes[i];
    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++)
      deviceStats.memoryHeaps[i].memoryHeap = memoryProperties.memoryHeaps[i];

  }
  return ret;
}

VK_LAYER_EXPORT void VKAPI_CALL MemoryTrack_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  auto &deviceStats = devices[device];
  uint64_t sum_device = 0, sum_host = 0;

  printf("Maximum usage by memory type index:\n");
  for (int i = 0; i < deviceStats.memoryTypes.size(); i++)
  {
    const auto &typeInfo = deviceStats.memoryTypes[i];
    printf(" %3d: %" PRIu64 " bytes (heap %u)\n", i,
           (uint64_t) typeInfo.maximumUsage, typeInfo.memoryType.heapIndex);
  }

  printf("Maximum usage by memory heap:\n");
  for (int i = 0; i < deviceStats.memoryHeaps.size(); i++)
  {
    const auto &heapInfo = deviceStats.memoryHeaps[i];
    printf(" %3d: %" PRIu64 " bytes\n", i, (uint64_t) heapInfo.maximumUsage);

    if (heapInfo.memoryHeap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      sum_device += heapInfo.maximumUsage;
    else
      sum_host += heapInfo.maximumUsage;
  }

  printf("Maximum device memory: %" PRIu64 " bytes\n", sum_device);
  printf("Maximum host memory: %" PRIu64 " bytes\n", sum_host);

  devices.erase(device);
  device_dispatch.erase(GetKey(device));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                               const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
  scoped_lock l(global_lock);
  VkResult res = device_dispatch[GetKey(device)].AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
  if (res == VK_SUCCESS)
  {
    allocations[*pMemory] = *pAllocateInfo;

    auto &memoryTypeInfo = devices[device].memoryTypes[pAllocateInfo->memoryTypeIndex];
    auto &memoryHeapInfo = devices[device].memoryHeaps[memoryTypeInfo.memoryType.heapIndex];

    memoryTypeInfo.currentUsage += pAllocateInfo->allocationSize;
    memoryHeapInfo.currentUsage += pAllocateInfo->allocationSize;
    if (memoryTypeInfo.currentUsage > memoryTypeInfo.maximumUsage)
      memoryTypeInfo.maximumUsage = memoryTypeInfo.currentUsage;
    if (memoryHeapInfo.currentUsage > memoryHeapInfo.maximumUsage)
      memoryHeapInfo.maximumUsage = memoryHeapInfo.currentUsage;
  }

  return res;
}

VK_LAYER_EXPORT void VKAPI_CALL MemoryTrack_FreeMemory(VkDevice device, VkDeviceMemory memory,
                                                       const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);

  const auto &allocInfo = allocations[memory];
  auto &memoryTypeInfo = devices[device].memoryTypes[allocInfo.memoryTypeIndex];
  auto &memoryHeapInfo = devices[device].memoryHeaps[memoryTypeInfo.memoryType.heapIndex];
  memoryTypeInfo.currentUsage -= allocInfo.allocationSize;
  memoryHeapInfo.currentUsage -= allocInfo.allocationSize;
  allocations.erase(memory);

  device_dispatch[GetKey(device)].FreeMemory(device, memory, pAllocator);
}

///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                       VkLayerProperties *pProperties)
{
  if(pPropertyCount) *pPropertyCount = 1;

  if(pProperties)
  {
    strcpy(pProperties->layerName, "VK_LAYER_NXT_MemoryTrack");
    strcpy(pProperties->description, "Layer to track and report Vulkan memory allocations");
    pProperties->implementationVersion = 1;
    pProperties->specVersion = VK_API_VERSION_1_0;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
  return MemoryTrack_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_EnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_NXT_MemoryTrack"))
    return VK_ERROR_LAYER_NOT_PRESENT;

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL MemoryTrack_EnumerateDeviceExtensionProperties(
                                     VkPhysicalDevice physicalDevice, const char *pLayerName,
                                     uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  // pass through any queries that aren't to us
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_NXT_MemoryTrack"))
  {
    if(physicalDevice == VK_NULL_HANDLE)
      return VK_SUCCESS;

    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer

#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&MemoryTrack_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL MemoryTrack_GetDeviceProcAddr(VkDevice device, const char *pName)
{
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(AllocateMemory);
  GETPROCADDR(FreeMemory);

  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL MemoryTrack_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(EnumerateInstanceLayerProperties);
  GETPROCADDR(EnumerateInstanceExtensionProperties);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);

  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(AllocateMemory);
  GETPROCADDR(FreeMemory);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}
