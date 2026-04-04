#pragma once

// ---------------------------------------------------------------------------
// api.hpp  -  top-level "lib.rs" equivalent: re-exports everything public
// ---------------------------------------------------------------------------

#include "client.hpp"
#include "error.hpp"
#include "prompt_cache.hpp"
#include "sse.hpp"
#include "types.hpp"
#include "providers/anthropic.hpp"
#include "providers/mod.hpp"
#include "providers/openai_compat.hpp"