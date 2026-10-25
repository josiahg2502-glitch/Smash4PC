// coreinit access to hardware registers and OS drivers.
//
// The title polls the legacy serial interface (interface 6, SI) through the
// kernel register calls, waiting for transfer-busy bits to clear. There is
// no such hardware here: registers read as idle and writes are dropped.

#include "kernel.h"

#include "cafe/export.h"

namespace cafe::os {
namespace {

uint32_t __OSReadRegister32Ex(uint32_t, uint32_t) { return 0; }
void __OSWriteRegister32Ex(uint32_t, uint32_t, uint32_t) {}
uint16_t OSReadRegister16(uint32_t, uint32_t) { return 0; }
void OSWriteRegister16(uint16_t, uint32_t, uint32_t) {}

// Drivers get callbacks on process transitions (foreground release and
// acquire); this runtime has none, so registration only succeeds.
int32_t OSDriver_Register(uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t OSDriver_Deregister(uint32_t, uint32_t) { return 0; }

} // namespace

CAFE_EXPORT(coreinit, __OSReadRegister32Ex, __OSReadRegister32Ex);
CAFE_EXPORT(coreinit, __OSWriteRegister32Ex, __OSWriteRegister32Ex);
CAFE_EXPORT(coreinit, OSReadRegister16, OSReadRegister16);
CAFE_EXPORT(coreinit, OSWriteRegister16, OSWriteRegister16);
CAFE_EXPORT(coreinit, OSDriver_Register, OSDriver_Register);
CAFE_EXPORT(coreinit, OSDriver_Deregister, OSDriver_Deregister);

} // namespace cafe::os
