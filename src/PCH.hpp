#pragma once

// IWYU pragma: begin_exports
#include "F4SE/F4SE.hpp"

#include "RE/Game.hpp"

#include "Common.hpp"

#if F4R_HAS_STREAMLINE
#include <sl.h>
#include <sl_dlss.h>
#include <sl_reflex.h>
#endif

#if F4R_HAS_FSR3
// FSR3 SDK
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#endif
// IWYU pragma: end_exports