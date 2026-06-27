#ifndef RAT_H
#define RAT_H

#include "IR/Module.h"

#include "CodeGen/Schedule.h"
#include "Target/Target.h"

#include "Pass/Verify.h"
#include "Support/PassManager.h"
#include "Support/PassRegistry.h"

#include "Pass/Emit/CEmitter.h"
#include "Pass/Emit/GraphEmitter.h"
#include "Pass/Emit/TextEmitter.h"
#include "Pass/Emit/X86Emitter.h"

#include "Pass/Opt/Fold.h"
#include "Pass/Opt/GVN.h"
#include "Pass/Opt/Inline.h"
#include "Pass/Opt/MemoryOpt.h"
#include "Pass/Opt/RenameSymbol.h"
#include "Pass/Opt/SCCP.h"
#include "Pass/Opt/SimplifyCFG.h"

#endif
