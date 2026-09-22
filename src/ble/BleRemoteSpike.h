#pragma once

// Stage 0 size/lifecycle spike for docs/ble-remote.md. Compiled only by the
// temporary x4-pro-blespike environment; no production path references it.
#if defined(CROSSINK_BLE_REMOTE_SPIKE) && CROSSINK_BLE_REMOTE_SPIKE

namespace BleRemoteSpike {

// Runs the whole probe synchronously from setup(): heap census, one
// init/advertise/deinit round trip, ten more cycles, then a bounded pairing
// and key-delivery window. Returns once the stack is torn down.
void run();

}  // namespace BleRemoteSpike

#endif
