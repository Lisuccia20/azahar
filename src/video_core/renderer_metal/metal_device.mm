#ifdef __APPLE__
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "metal_device.h"
#include "common/logging/log.h"

namespace Metal {

MetalDevice::MetalDevice() {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        LOG_CRITICAL(Render, "Nessun dispositivo Metal trovato!");
        return;
    }
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (!queue) {
        LOG_CRITICAL(Render, "Impossibile creare la Metal command queue!");
        return;
    }
    // Mantieni i riferimenti tramite void* (ARC li gestisce comunque)
    device_opaque = (__bridge_retained void*)dev;
    queue_opaque  = (__bridge_retained void*)queue;

    LOG_INFO(Render, "Metal device: {}", GetDriverName());
}

MetalDevice::~MetalDevice() {
    if (device_opaque) {
        id<MTLDevice> dev = (__bridge_transfer id<MTLDevice>)device_opaque;
        (void)dev;
        device_opaque = nullptr;
    }
    if (queue_opaque) {
        id<MTLCommandQueue> q = (__bridge_transfer id<MTLCommandQueue>)queue_opaque;
        (void)q;
        queue_opaque = nullptr;
    }
}

std::string MetalDevice::GetDriverName() const {
    if (!device_opaque) return "Unknown";
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device_opaque;
    return std::string([[dev name] UTF8String]);
}

bool MetalDevice::IsAppleSilicon() const {
    if (!device_opaque) return false;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device_opaque;
    return [dev supportsFamily:MTLGPUFamilyApple7];
}

} // namespace Metal
#endif // __APPLE__