#ifndef RAT_H
#define RAT_H

#include "IR/Module.h"

#include "CodeGen/Schedule.h"
#include "Target/Target.h"

#include "Pass/PassManager.h"
#include "Pass/PassRegistry.h"
#include "Pass/Verify.h"

#include "Pass/Emit/CEmitter.h"
#include "Pass/Emit/GraphEmitter.h"
#include "Pass/Emit/TextEmitter.h"

#include "Pass/Opt/Fold.h"
#include "Pass/Opt/GVN.h"
#include "Pass/Opt/Inline.h"
#include "Pass/Opt/MemoryOpt.h"
#include "Pass/Opt/SCCP.h"
#include "Pass/Opt/SimplifyCFG.h"

#endif
