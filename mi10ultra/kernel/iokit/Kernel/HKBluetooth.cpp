/* The native daemon discovers its public address by matching the service name
 * "bluetooth" and reading local-mac-address. H4 data uses the kernel-control
 * transport; this entry publishes only the controller's measured identity. */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>

extern "C" int umi_bluetooth_publish(const uint8_t address[6],const uint8_t commands[64]);
static IOService *bluetooth_service;
extern "C" void umi_bluetooth_set_ready(bool ready);
extern "C" void umi_bluetooth_set_ready(bool ready)
{
    if (bluetooth_service) bluetooth_service->setProperty("HKBluetoothReady", ready);
}
extern "C" int
umi_bluetooth_publish(const uint8_t address[6],const uint8_t commands[64])
{
    IOService *provider = IOService::getPlatform();
    IOService *service = new IOService;
    if (!provider || !service) { if (service) service->release(); return ENOMEM; }
    if (!service->init()) { service->release(); return ENOMEM; }
    OSData *mac = OSData::withBytes(address, 6);
    if (!mac) { service->release(); return ENOMEM; }
    service->setName("bluetooth");
    bool published = service->setProperty("local-mac-address", mac);
    mac->release();
    if (!published) { service->release(); return ENOMEM; }
    OSData *supported = OSData::withBytes(commands,64);
    if (!supported) { service->release(); return ENOMEM; }
    published = service->setProperty("HKBluetoothSupportedCommands",supported);
    supported->release();
    if (!published) { service->release(); return ENOMEM; }
    service->setProperty("transport-speed", 115200, 32);
    service->setProperty("compatible", "qca,qca6390");
    service->setProperty("HKBluetoothReady", false);
    if (!service->attach(provider)) { service->release(); return EIO; }
    if (!service->start(provider)) { service->detach(provider); service->release(); return EIO; }
    service->registerService();
    bluetooth_service = service;
    service->release(); /* Registry attachment owns the live service. */
    return 0;
}
