// The natives that give the JavaScript parts of the engine (Intl, Temporal) ICU's locale data and time zone rules.
#pragma once
#include "js.h"

namespace tb::js {

JSObject* icuNatives(Vm& vm);

}  // namespace tb::js
