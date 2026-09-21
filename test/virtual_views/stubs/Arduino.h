#pragma once
#include "HalStorage.h"
inline unsigned long millis() {
  const auto now = fixture::clock;
  fixture::clock += fixture::tick;
  return now;
}
