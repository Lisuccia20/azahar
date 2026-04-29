#pragma once
#ifdef __APPLE__

#include <string>

namespace Metal {

class MetalDevice {
public:
    MetalDevice();
    ~MetalDevice();

    void*       GetDevice()       const { return device_opaque; }
    void*       GetCommandQueue() const { return queue_opaque;  }
    std::string GetDriverName()   const;
    bool        IsAppleSilicon()  const;

private:
    void* device_opaque = nullptr;
    void* queue_opaque  = nullptr;
};

} // namespace Metal
#endif // __APPLE__