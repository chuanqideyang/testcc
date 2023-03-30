// Copyright (c) 2023. ByteDance Inc. All rights reserved.

#pragma once

#include "demo/utils/thirdparty/quiche/quic_clock.h"
#include "demo/utils/thirdparty/quiche/quic_time.h"


using Timepoint = quic::QuicTime;
using Duration = quic::QuicTime::Delta;
#ifdef PCDN_USE_NS3
#include "ns3clock.hpp"
using Clock = NS3Clock;
#else

#include "defaultclock.hpp"

using Clock = DefaultClock;
#endif
